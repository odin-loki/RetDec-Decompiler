/**
 * @file src/capstone2llvmir/riscv/riscv.cpp
 * @brief RISC-V implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <regex>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsRISCV.h>

#include "capstone2llvmir/riscv/riscv_impl.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

uint32_t csrLlvmRegId(uint16_t encoding)
{
	return static_cast<uint32_t>(RISCV_REG_ENDING) + 0x1000u + encoding;
}

uint32_t rvvGroupBase(uint32_t r)
{
	if (r >= RISCV_REG_V0 && r <= RISCV_REG_V31)
	{
		return r;
	}
	switch (r)
	{
		case RISCV_REG_V0M2:
		case RISCV_REG_V0M4:
		case RISCV_REG_V0M8:
			return RISCV_REG_V0;
		case RISCV_REG_V2M2:
			return RISCV_REG_V2;
		case RISCV_REG_V4M2:
		case RISCV_REG_V4M4:
			return RISCV_REG_V4;
		case RISCV_REG_V6M2:
			return RISCV_REG_V6;
		case RISCV_REG_V8M2:
		case RISCV_REG_V8M4:
		case RISCV_REG_V8M8:
			return RISCV_REG_V8;
		case RISCV_REG_V10M2:
			return RISCV_REG_V10;
		case RISCV_REG_V12M2:
		case RISCV_REG_V12M4:
			return RISCV_REG_V12;
		case RISCV_REG_V14M2:
			return RISCV_REG_V14;
		case RISCV_REG_V16M2:
		case RISCV_REG_V16M4:
		case RISCV_REG_V16M8:
			return RISCV_REG_V16;
		case RISCV_REG_V18M2:
			return RISCV_REG_V18;
		case RISCV_REG_V20M2:
		case RISCV_REG_V20M4:
			return RISCV_REG_V20;
		case RISCV_REG_V22M2:
			return RISCV_REG_V22;
		case RISCV_REG_V24M2:
		case RISCV_REG_V24M4:
		case RISCV_REG_V24M8:
			return RISCV_REG_V24;
		case RISCV_REG_V26M2:
			return RISCV_REG_V26;
		case RISCV_REG_V28M2:
		case RISCV_REG_V28M4:
			return RISCV_REG_V28;
		case RISCV_REG_V30M2:
			return RISCV_REG_V30;
		default:
			return RISCV_REG_INVALID;
	}
}

enum { VSEG_UNIT = 0, VSEG_STRIDE = 1, VSEG_INDEX = 2, VSEG_WHOLE = 3 };

bool parseVseg(
		cs_insn* i,
		unsigned& nf,
		unsigned& bits,
		bool& isStore,
		int& kind)
{
	const char* m = i->mnemonic;
	nf = 0;
	bits = 32;
	isStore = false;
	kind = VSEG_UNIT;
	if (m == nullptr || m[0] != 'v')
	{
		return false;
	}

	auto parseNfEew = [](const char* p, unsigned& nfo, unsigned& bitso, bool indexed) -> bool
	{
		if (p == nullptr || *p < '1' || *p > '8')
		{
			return false;
		}
		nfo = static_cast<unsigned>(*p - '0');
		++p;
		if (*p != 'e')
		{
			return false;
		}
		++p;
		if (indexed)
		{
			if (*p != 'i')
			{
				return false;
			}
			++p;
		}
		if (*p < '0' || *p > '9')
		{
			return false;
		}
		bitso = static_cast<unsigned>(std::strtoul(p, nullptr, 10));
		return (nfo >= 1 && nfo <= 8)
				&& (bitso == 8 || bitso == 16 || bitso == 32 || bitso == 64);
	};

	if (std::strncmp(m, "vlsseg", 6) == 0)
	{
		kind = VSEG_STRIDE;
		return parseNfEew(m + 6, nf, bits, false);
	}
	if (std::strncmp(m, "vssseg", 6) == 0)
	{
		isStore = true;
		kind = VSEG_STRIDE;
		return parseNfEew(m + 6, nf, bits, false);
	}
	if (std::strncmp(m, "vlseg", 5) == 0)
	{
		kind = VSEG_UNIT;
		return parseNfEew(m + 5, nf, bits, false);
	}
	if (std::strncmp(m, "vsseg", 5) == 0)
	{
		isStore = true;
		kind = VSEG_UNIT;
		return parseNfEew(m + 5, nf, bits, false);
	}
	if (std::strncmp(m, "vloxseg", 7) == 0 || std::strncmp(m, "vluxseg", 7) == 0)
	{
		kind = VSEG_INDEX;
		return parseNfEew(m + 7, nf, bits, true);
	}
	if (std::strncmp(m, "vsoxseg", 7) == 0 || std::strncmp(m, "vsuxseg", 7) == 0)
	{
		isStore = true;
		kind = VSEG_INDEX;
		return parseNfEew(m + 7, nf, bits, true);
	}
	if (m[1] == 'l'
			&& (m[2] == '1' || m[2] == '2' || m[2] == '4' || m[2] == '8')
			&& m[3] == 'r'
			&& m[4] == 'e')
	{
		kind = VSEG_WHOLE;
		nf = static_cast<unsigned>(m[2] - '0');
		bits = static_cast<unsigned>(std::strtoul(m + 5, nullptr, 10));
		return bits == 8 || bits == 16 || bits == 32 || bits == 64;
	}
	if (m[1] == 's'
			&& (m[2] == '1' || m[2] == '2' || m[2] == '4' || m[2] == '8')
			&& m[3] == 'r'
			&& (m[4] == '.' || m[4] == '\0'))
	{
		isStore = true;
		kind = VSEG_WHOLE;
		nf = static_cast<unsigned>(m[2] - '0');
		bits = 8;
		return true;
	}
	return false;
}

llvm::Value* splatRvvFromScalar(llvm::Value* s, llvm::IRBuilder<>& irb, llvm::Type* i32ty)
{
	if (s->getType()->isFloatingPointTy())
	{
		auto* f32 = llvm::Type::getFloatTy(s->getContext());
		if (s->getType() != f32)
		{
			s = irb.CreateFPCast(s, f32);
		}
		s = irb.CreateBitCast(s, i32ty);
	}
	return s;
}

llvm::Value* bf16LanesToF32(llvm::Value* v, llvm::IRBuilder<>& irb)
{
	auto* i16ty = llvm::ScalableVectorType::get(irb.getInt16Ty(), 8);
	auto* fty = llvm::ScalableVectorType::get(irb.getFloatTy(), 4);
	auto* packed = irb.CreateBitCast(v, i16ty);
	llvm::Value* dest = llvm::PoisonValue::get(fty);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* h = irb.CreateExtractElement(packed, irb.getInt32(lane));
		auto* w = irb.CreateShl(
				irb.CreateZExt(h, irb.getInt32Ty()),
				irb.getInt32(16));
		dest = irb.CreateInsertElement(
				dest,
				irb.CreateBitCast(w, irb.getFloatTy()),
				irb.getInt32(lane));
	}
	return dest;
}

llvm::Value* f32LanesToBf16(llvm::Value* fp, llvm::IRBuilder<>& irb, llvm::Type* vregTy)
{
	auto* i16ty = llvm::ScalableVectorType::get(irb.getInt16Ty(), 8);
	llvm::Value* dest = llvm::Constant::getNullValue(i16ty);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* bits = irb.CreateBitCast(
				irb.CreateExtractElement(fp, irb.getInt32(lane)),
				irb.getInt32Ty());
		auto* h = irb.CreateTrunc(
				irb.CreateLShr(bits, irb.getInt32(16)),
				irb.getInt16Ty());
		dest = irb.CreateInsertElement(dest, h, irb.getInt32(lane));
	}
	return irb.CreateBitCast(dest, vregTy);
}

llvm::Value* bf16ScalarToF32Vec(llvm::Value* s, llvm::IRBuilder<>& irb)
{
	if (s->getType()->isVectorTy())
	{
		s = irb.CreateExtractElement(s, irb.getInt32(0));
	}
	if (s->getType()->isFloatingPointTy())
	{
		s = irb.CreateBitCast(
				s,
				irb.getIntNTy(s->getType()->getPrimitiveSizeInBits()));
	}
	if (!s->getType()->isIntegerTy(32))
	{
		s = irb.CreateZExtOrTrunc(s, irb.getInt32Ty());
	}
	auto* h = irb.CreateTrunc(s, irb.getInt16Ty());
	auto* w = irb.CreateShl(irb.CreateZExt(h, irb.getInt32Ty()), irb.getInt32(16));
	auto* f = irb.CreateBitCast(w, irb.getFloatTy());
	auto* fty = llvm::ScalableVectorType::get(irb.getFloatTy(), 4);
	return irb.CreateVectorSplat(
			llvm::cast<llvm::VectorType>(fty)->getElementCount(),
			f);
}

// OP-VE (opcode 0x77, funct3 = OPMVV) is Zvkned/Zvknh/Zvksed/Zvksh/Zvkg.
// Capstone 6 does not report these ids under the modes the tests set, so the
// llvm.riscv.* call was never created. vs1 is an opcode field for the .vv/.vs
// forms and an immediate for the .vi forms.
bool riscvRewriteVcrypto(cs_insn* i, cs_riscv* ri)
{
	if (i->size < 4)
	{
		return false;
	}
	const uint32_t w = static_cast<uint32_t>(i->bytes[0]) | (static_cast<uint32_t>(i->bytes[1]) << 8)
					 | (static_cast<uint32_t>(i->bytes[2]) << 16) | (static_cast<uint32_t>(i->bytes[3]) << 24);
	if ((w & 0x7fu) != 0x77u || ((w >> 12) & 7u) != 2u)
	{
		return false;
	}
	const unsigned f6 = (w >> 26) & 63u;
	const unsigned vs1 = (w >> 15) & 31u;
	unsigned id = 0;
	unsigned kind = 0;
	switch ((f6 << 5) | vs1)
	{
	case (0b101000u << 5) | 0b00000u: id = RISCV_INS_VAESDM_VV; break;
	case (0b101001u << 5) | 0b00000u: id = RISCV_INS_VAESDM_VS; break;
	case (0b101000u << 5) | 0b00001u: id = RISCV_INS_VAESDF_VV; break;
	case (0b101001u << 5) | 0b00001u: id = RISCV_INS_VAESDF_VS; break;
	case (0b101000u << 5) | 0b00010u: id = RISCV_INS_VAESEM_VV; break;
	case (0b101001u << 5) | 0b00010u: id = RISCV_INS_VAESEM_VS; break;
	case (0b101000u << 5) | 0b00011u: id = RISCV_INS_VAESEF_VV; break;
	case (0b101001u << 5) | 0b00011u: id = RISCV_INS_VAESEF_VS; break;
	case (0b101001u << 5) | 0b00111u: id = RISCV_INS_VAESZ_VS; break;
	case (0b101000u << 5) | 0b10000u: id = RISCV_INS_VSM4R_VV; break;
	case (0b101001u << 5) | 0b10000u: id = RISCV_INS_VSM4R_VS; break;
	case (0b101000u << 5) | 0b10001u: id = RISCV_INS_VGMUL_VV; break;
	default: break;
	}
	if (id == 0)
	{
		kind = 1;
		switch (f6)
		{
		case 0b101110u: id = RISCV_INS_VSHA2CH_VV; break;
		case 0b101111u: id = RISCV_INS_VSHA2CL_VV; break;
		case 0b101101u: id = RISCV_INS_VSHA2MS_VV; break;
		case 0b101100u: id = RISCV_INS_VGHSH_VV; break;
		case 0b100000u: id = RISCV_INS_VSM3ME_VV; break;
		case 0b100010u:
			id = RISCV_INS_VAESKF1_VI;
			kind = 2;
			break;
		case 0b101010u:
			id = RISCV_INS_VAESKF2_VI;
			kind = 2;
			break;
		case 0b100001u:
			id = RISCV_INS_VSM4K_VI;
			kind = 2;
			break;
		case 0b101011u:
			id = RISCV_INS_VSM3C_VI;
			kind = 2;
			break;
		default: return false;
		}
	}
	if (i->id == id || (i->is_alias && static_cast<unsigned>(i->alias_id) == id))
	{
		return false;
	}

	auto setReg = [&](unsigned n, unsigned regn) {
		std::memset(&ri->operands[n], 0, sizeof(ri->operands[n]));
		ri->operands[n].type = RISCV_OP_REG;
		ri->operands[n].reg = static_cast<unsigned>(RISCV_REG_V0) + regn;
	};
	setReg(0, (w >> 7) & 31u);
	setReg(1, (w >> 20) & 31u);
	if (kind == 0)
	{
		ri->op_count = 2;
	}
	else if (kind == 1)
	{
		setReg(2, vs1);
		ri->op_count = 3;
	}
	else
	{
		std::memset(&ri->operands[2], 0, sizeof(ri->operands[2]));
		ri->operands[2].type = RISCV_OP_IMM;
		ri->operands[2].imm = static_cast<int64_t>(vs1);
		ri->op_count = 3;
	}
	i->id = id;
	i->is_alias = false;
	return true;
}

} // namespace


Capstone2LlvmIrTranslatorRiscv_impl::Capstone2LlvmIrTranslatorRiscv_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(
				CS_ARCH_RISCV,
				basic,
				static_cast<cs_mode>(
						static_cast<unsigned>(extra)
						| static_cast<unsigned>(CS_MODE_RISCV_V)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBA)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBB)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBKB)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBKX)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBC)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBS)
						| static_cast<unsigned>(CS_MODE_RISCV_VENTANA)),
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

bool Capstone2LlvmIrTranslatorRiscv_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_RISCV32
			|| m == CS_MODE_RISCV64;
}

bool Capstone2LlvmIrTranslatorRiscv_impl::isAllowedExtraMode(cs_mode m)
{
	unsigned ext = static_cast<unsigned>(CS_MODE_RISCVC)
			| static_cast<unsigned>(CS_MODE_RISCV_FD)
			| static_cast<unsigned>(CS_MODE_RISCV_A)
			| static_cast<unsigned>(CS_MODE_RISCV_V)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBA)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBB)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBKB)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBKX)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBC)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBS)
			| static_cast<unsigned>(CS_MODE_RISCV_VENTANA)
			| static_cast<unsigned>(CS_MODE_RISCV_ZCMP_ZCMT_ZCE)
			| static_cast<unsigned>(CS_MODE_RISCV_THEAD)
			| static_cast<unsigned>(CS_MODE_RISCV_COREV);
	auto stripped = static_cast<cs_mode>(static_cast<unsigned>(m) & ~ext);
	return stripped == CS_MODE_LITTLE_ENDIAN
			|| stripped == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorRiscv_impl::getArchByteSize()
{
	return isXlen64() ? 8 : 4;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorRiscv_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorRiscv_impl::generateDataLayout()
{
	if (isXlen64())
	{
		_module->setDataLayout("e-p:64:64:64-i8:8:32-i16:16:32-i64:64-n32:64-S128");
	}
	else
	{
		_module->setDataLayout("e-p:32:32:32-f80:32:32");
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		// Scalable V0–V31 stay lazy: llvmir-emul cannot constant-fold
		// <vscale x N x T> zeroinit, and that would abort every RISC-V test.
		if (p.second->isVectorTy())
		{
			continue;
		}
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorRiscv_impl::getCarryRegister()
{
	return RISCV_REG_INVALID;
}

typename Capstone2LlvmIrTranslator_impl<cs_riscv, cs_riscv_op>::TranslationResult
Capstone2LlvmIrTranslatorRiscv_impl::translate(
	const uint8_t* bytes,
	std::size_t size,
	retdec::common::Address a,
	llvm::IRBuilder<>& irb,
	std::size_t count,
	bool stopOnBranch)
{
	auto res =
		Capstone2LlvmIrTranslator_impl<cs_riscv, cs_riscv_op>::translate(bytes, size, a, irb, count, stopOnBranch);
	// Decoder tables miss OP-VE. The word never becomes an instruction, so
	// the id rewrite in translateInstruction has nothing to correct.
	if (res.count != 0 || size < 4)
	{
		return res;
	}
	cs_insn* insn = cs_malloc(_handle);
	if (!insn || !insn->detail)
	{
		if (insn)
		{
			cs_free(insn, 1);
		}
		return res;
	}
	insn->size = 4;
	std::memcpy(insn->bytes, bytes, 4);
	insn->address = a;
	if (riscvRewriteVcrypto(insn, &insn->detail->riscv))
	{
		translateInstruction(insn, irb);
		res.count = 1;
		res.size = 4;
	}
	cs_free(insn, 1);
	return res;
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateInstruction(cs_insn* i, llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_riscv* ri = &d->riscv;
	riscvRewriteVcrypto(i, ri);

	// Capstone 6 keeps the real id (ADDI/JAL/CSRRS) and puts nop/jal/frflags
	// in alias_id. Prefer a dedicated alias translator (e.g. nop vs addi x0).
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
		(this->*f)(i, ri, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, ri, irb);
	}
}

//
//==============================================================================
// RISC-V-specific methods.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorRiscv_impl::isXlen64() const
{
	return _origBasicMode == CS_MODE_RISCV64;
}

bool Capstone2LlvmIrTranslatorRiscv_impl::isRvvReg(uint32_t r) const
{
	return r >= RISCV_REG_V0 && r <= RISCV_REG_V31;
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::rvvRegType()
{
	return llvm::ScalableVectorType::get(
			llvm::Type::getInt32Ty(_module->getContext()),
			4);
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::rvvMemVecType(unsigned elemBits)
{
	auto& ctx = _module->getContext();
	switch (elemBits)
	{
		case 8:
			return llvm::ScalableVectorType::get(llvm::Type::getInt8Ty(ctx), 16);
		case 16:
			return llvm::ScalableVectorType::get(llvm::Type::getInt16Ty(ctx), 8);
		case 64:
			return llvm::ScalableVectorType::get(llvm::Type::getInt64Ty(ctx), 2);
		default:
			return rvvRegType();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::splatToRvv(
		llvm::Value* s,
		llvm::IRBuilder<>& irb)
{
	auto* vecTy = llvm::cast<llvm::VectorType>(rvvRegType());
	if (s->getType()->isVectorTy())
	{
		if (s->getType() != vecTy)
		{
			return irb.CreateBitCast(s, vecTy);
		}
		return s;
	}
	// A scalar FP register is the lane's bits. Sign-extending the float aborts.
	if (!s->getType()->isIntOrIntVectorTy())
	{
		unsigned bits = s->getType()->getPrimitiveSizeInBits();
		if (bits == 0)
		{
			bits = vecTy->getElementType()->getIntegerBitWidth();
		}
		s = irb.CreateBitCast(s, irb.getIntNTy(bits));
	}
	s = irb.CreateSExtOrTrunc(s, vecTy->getElementType());
	return irb.CreateVectorSplat(vecTy->getElementCount(), s);
}

llvm::GlobalVariable* Capstone2LlvmIrTranslatorRiscv_impl::ensureRvvRegister(uint32_t r)
{
	if (auto* existing = getRegister(r))
	{
		return existing;
	}

	auto* rt = getRegisterType(r);
	auto* gv = new llvm::GlobalVariable(
			*_module,
			rt,
			false,
			_regLt,
			llvm::Constant::getNullValue(rt),
			getRegisterName(r));
	_llvm2CapstoneRegs[gv] = r;
	_capstone2LlvmRegs[r] = gv;
	return gv;
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::rvvMemAddr(
		cs_riscv_op& op,
		llvm::IRBuilder<>& irb)
{
	if (op.type == RISCV_OP_MEM)
	{
		return loadOp(op, irb, nullptr, true);
	}
	if (op.type == RISCV_OP_REG)
	{
		return loadRegister(op.reg, irb);
	}
	return loadOp(op, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::getCurrentPc(cs_insn* i)
{
	return llvm::ConstantInt::get(getDefaultType(), i->address);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::pcRelativeTarget(
		cs_insn* i,
		llvm::Value* offset,
		llvm::IRBuilder<>& irb)
{
	auto* pc = getCurrentPc(i);
	offset = generateTypeConversion(irb, offset, pc->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	// Capstone 6: need_effective_addr means the IMM is still a displacement.
	// When the flag is clear, IMM is already the effective target
	// (beq +8 at 0x1000 arrives as 0x1008, not 8).
	if (i->detail && i->detail->riscv.need_effective_addr)
	{
		return irb.CreateAdd(pc, offset);
	}
	return offset;
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::maskShiftAmount(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* amount)
{
	unsigned mask = val->getType()->getIntegerBitWidth() - 1;
	amount = irb.CreateZExtOrTrunc(amount, val->getType());
	return irb.CreateAnd(amount, llvm::ConstantInt::get(val->getType(), mask));
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::narrowToWord(
		llvm::IRBuilder<>& irb,
		llvm::Value* val)
{
	return irb.CreateTrunc(val, irb.getInt32Ty());
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::widenFromWord(
		llvm::IRBuilder<>& irb,
		llvm::Value* val)
{
	return irb.CreateSExt(val, getDefaultType());
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == RISCV_REG_INVALID)
	{
		return nullptr;
	}

	if (r == RISCV_REG_PC)
	{
		auto* pc = getCurrentPc(_insn);
		return generateTypeConversion(irb, pc, dstType, ct);
	}

	if (r == RISCV_REG_X0)
	{
		auto* z = llvm::ConstantInt::get(getDefaultType(), 0);
		return generateTypeConversion(irb, z, dstType, ct);
	}

	if (isRvvReg(r))
	{
		ensureRvvRegister(r);
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);
	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::loadOp(
		cs_riscv_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case RISCV_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case RISCV_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::getSigned(t, op.imm);
		}
		case RISCV_OP_CSR:
		{
			auto* v = loadCsr(op.csr, irb);
			return v ? v : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case RISCV_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
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
		case RISCV_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::Instruction* Capstone2LlvmIrTranslatorRiscv_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == RISCV_REG_INVALID
			|| r == RISCV_REG_PC
			|| r == RISCV_REG_X0)
	{
		return nullptr;
	}

	if (isRvvReg(r))
	{
		ensureRvvRegister(r);
	}

	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, llvmReg->getValueType());
	return s;
}

llvm::Instruction* Capstone2LlvmIrTranslatorRiscv_impl::storeOp(
		cs_riscv_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case RISCV_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case RISCV_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
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

			return storeIntPtr(irb, val, addr, val->getType());
		}
		case RISCV_OP_IMM:
		case RISCV_OP_INVALID:
		default:
		{
			throw GenericError("should not be possible");
		}
	}
}

bool Capstone2LlvmIrTranslatorRiscv_impl::isOperandRegister(cs_riscv_op& op)
{
	return op.type == RISCV_OP_REG;
}

//
//==============================================================================
// RISC-V instruction translation methods.
//==============================================================================
//

/**
 * RISCV_INS_LUI, RISCV_INS_C_LUI
 * rd = sext(imm[31:12] << 12)
 * Capstone reports the 20-bit U-immediate, not the already-shifted value.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLui(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	op1 = loadOp(ri->operands[1], irb);
	auto* i32 = irb.getInt32Ty();
	auto* u = irb.CreateShl(irb.CreateZExtOrTrunc(op1, i32), llvm::ConstantInt::get(i32, 12));
	storeOp(ri->operands[0], u, irb);
}

/**
 * RISCV_INS_AUIPC
 * rd = pc + sext(imm[31:12] << 12)
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateAuipc(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	op1 = loadOp(ri->operands[1], irb);
	auto* i32 = irb.getInt32Ty();
	auto* u = irb.CreateShl(irb.CreateZExtOrTrunc(op1, i32), llvm::ConstantInt::get(i32, 12));
	u = generateTypeConversion(irb, u, getDefaultType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* add = irb.CreateAdd(getCurrentPc(i), u);
	storeOp(ri->operands[0], add, irb);
}

/**
 * RISCV_INS_JAL, RISCV_INS_C_J, RISCV_INS_C_JAL
 * rd = pc+size; pc += offset. rd is x0 / omitted for C.J.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateJal(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* offset = nullptr;
	uint32_t rd = RISCV_REG_X0;

	uint64_t alias = i->is_alias ? i->alias_id : 0;
	bool isJ = i->id == RISCV_INS_C_J || i->id == RISCV_INS_ALIAS_J
			|| alias == RISCV_INS_ALIAS_J;
	bool isJalAlias = i->id == RISCV_INS_C_JAL || i->id == RISCV_INS_ALIAS_JAL
			|| alias == RISCV_INS_ALIAS_JAL;

	if (isJ)
	{
		EXPECT_IS_UNARY(i, ri, irb);
		offset = loadOp(ri->operands[0], irb);
	}
	else if (isJalAlias || ri->op_count == 1)
	{
		if (ri->op_count == 1)
		{
			offset = loadOp(ri->operands[0], irb);
			rd = RISCV_REG_RA;
		}
		else
		{
			EXPECT_IS_BINARY(i, ri, irb);
			if (ri->operands[0].type == RISCV_OP_REG)
			{
				rd = ri->operands[0].reg;
			}
			offset = loadOp(ri->operands[1], irb);
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, ri, irb);
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rd = ri->operands[0].reg;
		}
		offset = loadOp(ri->operands[1], irb);
	}

	auto* target = pcRelativeTarget(i, offset, irb);
	if (rd != RISCV_REG_X0)
	{
		storeRegister(rd, getNextInsnAddress(i), irb);
		generateCallFunctionCall(irb, target);
	}
	else
	{
		generateBranchFunctionCall(irb, target);
	}
}

/**
 * RISCV_INS_JALR, RISCV_INS_C_JR, RISCV_INS_C_JALR
 * rd = pc+size; pc = (rs1 + imm) & ~1.
 * ret: rd=x0, rs1=ra.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateJalr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* target = nullptr;
	uint32_t rd = RISCV_REG_X0;
	uint32_t rs1 = RISCV_REG_INVALID;

	uint64_t alias = i->is_alias ? i->alias_id : 0;
	bool isJr = i->id == RISCV_INS_C_JR || i->id == RISCV_INS_ALIAS_JR
			|| alias == RISCV_INS_ALIAS_JR;
	bool isJalrAlias = i->id == RISCV_INS_C_JALR || i->id == RISCV_INS_ALIAS_JALR
			|| alias == RISCV_INS_ALIAS_JALR;
	bool isRet = i->id == RISCV_INS_ALIAS_RET || alias == RISCV_INS_ALIAS_RET;

	if (ri->op_count < 3 && (isJr || isJalrAlias || isRet || ri->op_count <= 1))
	{
		if (ri->op_count == 0)
		{
			rs1 = RISCV_REG_RA;
			target = loadRegister(RISCV_REG_RA, irb);
		}
		else if (ri->op_count == 1)
		{
			target = loadOp(ri->operands[0], irb, nullptr, ri->operands[0].type == RISCV_OP_MEM);
			if (ri->operands[0].type == RISCV_OP_REG)
			{
				rs1 = ri->operands[0].reg;
			}
			else if (ri->operands[0].type == RISCV_OP_MEM)
			{
				rs1 = ri->operands[0].mem.base;
			}
		}
		else if (ri->operands[0].type == RISCV_OP_REG
				&& ri->operands[1].type == RISCV_OP_IMM)
		{
			rs1 = ri->operands[0].reg;
			op1 = loadOp(ri->operands[0], irb);
			op2 = loadOp(ri->operands[1], irb);
			op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
			target = irb.CreateAdd(op1, op2);
		}
		else
		{
			EXPECT_IS_UNARY(i, ri, irb);
			target = loadOp(ri->operands[0], irb);
			if (ri->operands[0].type == RISCV_OP_REG)
			{
				rs1 = ri->operands[0].reg;
			}
		}
		if (isJalrAlias || (!isJr && !isRet && i->id == RISCV_INS_JALR))
		{
			rd = RISCV_REG_RA;
		}
	}
	else if (ri->op_count == 2 && ri->operands[1].type == RISCV_OP_MEM)
	{
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rd = ri->operands[0].reg;
		}
		rs1 = ri->operands[1].mem.base;
		target = loadOp(ri->operands[1], irb, nullptr, /*lea=*/true);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ri, irb);
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rd = ri->operands[0].reg;
		}
		if (ri->operands[1].type == RISCV_OP_REG)
		{
			rs1 = ri->operands[1].reg;
		}
		op1 = loadOp(ri->operands[1], irb);
		op2 = loadOp(ri->operands[2], irb);
		op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		target = irb.CreateAdd(op1, op2);
	}

	auto* one = llvm::ConstantInt::get(target->getType(), 1);
	target = irb.CreateAnd(target, irb.CreateNot(one));

	if (rd != RISCV_REG_X0)
	{
		storeRegister(rd, getNextInsnAddress(i), irb);
		generateCallFunctionCall(irb, target);
	}
	else if (rs1 == RISCV_REG_RA)
	{
		generateReturnFunctionCall(irb, target);
	}
	else
	{
		generateBranchFunctionCall(irb, target);
	}
}

/**
 * BEQ/BNE/BLT/BGE/BLTU/BGEU, C_BEQZ, C_BNEZ.
 * Capstone 6 IMM is the effective target unless need_effective_addr is set.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBranch(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* cond = nullptr;
	llvm::Value* offset = nullptr;

	if (i->id == RISCV_INS_C_BEQZ
			|| i->id == RISCV_INS_C_BNEZ
			|| i->id == RISCV_INS_ALIAS_BEQZ
			|| i->id == RISCV_INS_ALIAS_BNEZ
			|| i->id == RISCV_INS_ALIAS_BLEZ
			|| i->id == RISCV_INS_ALIAS_BGEZ
			|| i->id == RISCV_INS_ALIAS_BLTZ
			|| i->id == RISCV_INS_ALIAS_BGTZ)
	{
		EXPECT_IS_BINARY(i, ri, irb);
		op0 = loadOp(ri->operands[0], irb);
		offset = loadOp(ri->operands[1], irb);
		auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
		switch (i->id)
		{
			case RISCV_INS_C_BEQZ:
			case RISCV_INS_ALIAS_BEQZ:
				cond = irb.CreateICmpEQ(op0, zero);
				break;
			case RISCV_INS_C_BNEZ:
			case RISCV_INS_ALIAS_BNEZ:
				cond = irb.CreateICmpNE(op0, zero);
				break;
			case RISCV_INS_ALIAS_BLEZ:
				cond = irb.CreateICmpSLE(op0, zero);
				break;
			case RISCV_INS_ALIAS_BGEZ:
				cond = irb.CreateICmpSGE(op0, zero);
				break;
			case RISCV_INS_ALIAS_BLTZ:
				cond = irb.CreateICmpSLT(op0, zero);
				break;
			case RISCV_INS_ALIAS_BGTZ:
				cond = irb.CreateICmpSGT(op0, zero);
				break;
			default:
				throw GenericError("Unhandled insn ID in translateBranch().");
		}
	}
	else
	{
		EXPECT_IS_TERNARY(i, ri, irb);
		op0 = loadOp(ri->operands[0], irb);
		op1 = loadOp(ri->operands[1], irb);
		op1 = generateTypeConversion(irb, op1, op0->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		offset = loadOp(ri->operands[2], irb);

		switch (i->id)
		{
			case RISCV_INS_BEQ: cond = irb.CreateICmpEQ(op0, op1); break;
			case RISCV_INS_BNE: cond = irb.CreateICmpNE(op0, op1); break;
			case RISCV_INS_BLT: cond = irb.CreateICmpSLT(op0, op1); break;
			case RISCV_INS_BGE: cond = irb.CreateICmpSGE(op0, op1); break;
			case RISCV_INS_BLTU: cond = irb.CreateICmpULT(op0, op1); break;
			case RISCV_INS_BGEU: cond = irb.CreateICmpUGE(op0, op1); break;
			default:
				throw GenericError("Unhandled insn ID in translateBranch().");
		}
	}

	generateCondBranchFunctionCall(irb, cond, pcRelativeTarget(i, offset, irb));
}

/**
 * LB/LBU/LH/LHU/LW/LWU/LD, C_LW/C_LWSP/C_LD/C_LDSP, Zcb C_LBU/C_LH/C_LHU
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLoad(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	llvm::Type* ty = nullptr;
	eOpConv ct = eOpConv::THROW;

	switch (i->id)
	{
		case RISCV_INS_LB:
			ty = irb.getInt8Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LBU:
		case RISCV_INS_C_LBU:
			ty = irb.getInt8Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LH:
		case RISCV_INS_C_LH:
			ty = irb.getInt16Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LHU:
		case RISCV_INS_C_LHU:
			ty = irb.getInt16Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LW:
		case RISCV_INS_C_LW:
		case RISCV_INS_C_LWSP:
			ty = irb.getInt32Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LWU:
			ty = irb.getInt32Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LD:
		case RISCV_INS_C_LD:
		case RISCV_INS_C_LDSP:
			ty = irb.getInt64Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateLoad().");
	}

	op1 = loadOp(ri->operands[1], irb, ty);
	storeOp(ri->operands[0], op1, irb, ct);
}

/**
 * SB/SH/SW/SD, C_SW/C_SWSP/C_SD/C_SDSP, Zcb C_SB/C_SH
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateStore(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_SB:
		case RISCV_INS_C_SB:
			ty = irb.getInt8Ty();
			break;
		case RISCV_INS_SH:
		case RISCV_INS_C_SH:
			ty = irb.getInt16Ty();
			break;
		case RISCV_INS_SW:
		case RISCV_INS_C_SW:
		case RISCV_INS_C_SWSP:
			ty = irb.getInt32Ty();
			break;
		case RISCV_INS_SD:
		case RISCV_INS_C_SD:
		case RISCV_INS_C_SDSP:
			ty = irb.getInt64Ty();
			break;
		default:
			throw GenericError("Unhandled insn ID in translateStore().");
	}

	op0 = loadOp(ri->operands[0], irb);
	op0 = irb.CreateZExtOrTrunc(op0, ty);
	storeOp(ri->operands[1], op0, irb);
}

std::pair<llvm::Value*, llvm::Value*> Capstone2LlvmIrTranslatorRiscv_impl::loadAluSrc(
		cs_riscv* ri,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (ri->op_count == 2)
	{
		// Compressed: rd is also the first source.
		auto* a = loadOp(ri->operands[0], irb);
		auto* b = loadOp(ri->operands[1], irb);
		b = generateTypeConversion(irb, b, a->getType(), ct);
		return {a, b};
	}

	auto* a = loadOp(ri->operands[1], irb);
	auto* b = loadOp(ri->operands[2], irb);
	b = generateTypeConversion(irb, b, a->getType(), ct);
	return {a, b};
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateAdd(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateAdd(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSub(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateSub(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateAnd(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateAnd(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateOr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateOr(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateXor(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateXor(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSlt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* slt = irb.CreateZExt(irb.CreateICmpSLT(op1, op2), getDefaultType());
	storeOp(ri->operands[0], slt, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSltu(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ult = irb.CreateZExt(irb.CreateICmpULT(op1, op2), getDefaultType());
	storeOp(ri->operands[0], ult, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSll(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateShl(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSrl(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateLShr(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSra(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateAShr(op1, op2), irb);
}

/**
 * RV64I OP-32 / OP-IMM-32: ADDW, SUBW, SLLW, SRLW, SRAW,
 * ADDIW, SLLIW, SRLIW, SRAIW, C_ADDIW, C_ADDW, C_SUBW.
 * Compute in i32, sign-extend to XLEN.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateOp32(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(irb, op1);
	op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());

	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_ADDW:
		case RISCV_INS_ADDIW:
		case RISCV_INS_C_ADDIW:
		case RISCV_INS_C_ADDW:
			res = irb.CreateAdd(op1, op2);
			break;
		case RISCV_INS_SUBW:
		case RISCV_INS_C_SUBW:
			res = irb.CreateSub(op1, op2);
			break;
		case RISCV_INS_SLLW:
		case RISCV_INS_SLLIW:
			op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 31));
			res = irb.CreateShl(op1, op2);
			break;
		case RISCV_INS_SRLW:
		case RISCV_INS_SRLIW:
			op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 31));
			res = irb.CreateLShr(op1, op2);
			break;
		case RISCV_INS_SRAW:
		case RISCV_INS_SRAIW:
			op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 31));
			res = irb.CreateAShr(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateOp32().");
	}

	storeOp(ri->operands[0], widenFromWord(irb, res), irb);
}

/**
 * RISCV_INS_C_LI: rd = imm
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLi(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	storeOp(ri->operands[0], op1, irb);
}

/**
 * RISCV_INS_C_MV: rd = rs2
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	storeOp(ri->operands[0], op1, irb);
}

/**
 * RISCV_INS_ECALL, RISCV_INS_C_EBREAK is not this.
 * Environment call → translator pseudo-call (continuation at pc+size).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateEcall(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	(void)ri;
	generateCallFunctionCall(irb, getNextInsnAddress(i));
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateNop(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	(void)i;
	(void)ri;
	(void)irb;
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFence(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	(void)i;
	(void)ri;
	irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::amoAddress(
		cs_riscv_op& op,
		llvm::IRBuilder<>& irb)
{
	if (op.type == RISCV_OP_MEM)
	{
		auto* base = loadRegister(op.mem.base, irb);
		if (base == nullptr)
		{
			return llvm::ConstantInt::getSigned(getDefaultType(), op.mem.disp);
		}
		if (op.mem.disp == 0)
		{
			return base;
		}
		auto* disp = llvm::ConstantInt::getSigned(base->getType(), op.mem.disp);
		return irb.CreateAdd(base, disp);
	}
	return loadOp(op, irb, nullptr, /*lea=*/true);
}

llvm::AtomicOrdering Capstone2LlvmIrTranslatorRiscv_impl::amoOrdering(unsigned id) const
{
	switch (id)
	{
		case RISCV_INS_AMOADD_W_AQ:
		case RISCV_INS_AMOADD_D_AQ:
		case RISCV_INS_AMOAND_W_AQ:
		case RISCV_INS_AMOAND_D_AQ:
		case RISCV_INS_AMOOR_W_AQ:
		case RISCV_INS_AMOOR_D_AQ:
		case RISCV_INS_AMOXOR_W_AQ:
		case RISCV_INS_AMOXOR_D_AQ:
		case RISCV_INS_AMOSWAP_W_AQ:
		case RISCV_INS_AMOSWAP_D_AQ:
		case RISCV_INS_AMOMAX_W_AQ:
		case RISCV_INS_AMOMAX_D_AQ:
		case RISCV_INS_AMOMAXU_W_AQ:
		case RISCV_INS_AMOMAXU_D_AQ:
		case RISCV_INS_AMOMIN_W_AQ:
		case RISCV_INS_AMOMIN_D_AQ:
		case RISCV_INS_AMOMINU_W_AQ:
		case RISCV_INS_AMOMINU_D_AQ:
		case RISCV_INS_LR_W_AQ:
		case RISCV_INS_LR_D_AQ:
		case RISCV_INS_SC_W_AQ:
		case RISCV_INS_SC_D_AQ:
			return llvm::AtomicOrdering::Acquire;
		case RISCV_INS_AMOADD_W_RL:
		case RISCV_INS_AMOADD_D_RL:
		case RISCV_INS_AMOAND_W_RL:
		case RISCV_INS_AMOAND_D_RL:
		case RISCV_INS_AMOOR_W_RL:
		case RISCV_INS_AMOOR_D_RL:
		case RISCV_INS_AMOXOR_W_RL:
		case RISCV_INS_AMOXOR_D_RL:
		case RISCV_INS_AMOSWAP_W_RL:
		case RISCV_INS_AMOSWAP_D_RL:
		case RISCV_INS_AMOMAX_W_RL:
		case RISCV_INS_AMOMAX_D_RL:
		case RISCV_INS_AMOMAXU_W_RL:
		case RISCV_INS_AMOMAXU_D_RL:
		case RISCV_INS_AMOMIN_W_RL:
		case RISCV_INS_AMOMIN_D_RL:
		case RISCV_INS_AMOMINU_W_RL:
		case RISCV_INS_AMOMINU_D_RL:
		case RISCV_INS_LR_W_RL:
		case RISCV_INS_LR_D_RL:
		case RISCV_INS_SC_W_RL:
		case RISCV_INS_SC_D_RL:
			return llvm::AtomicOrdering::Release;
		case RISCV_INS_AMOADD_W_AQ_RL:
		case RISCV_INS_AMOADD_D_AQ_RL:
		case RISCV_INS_AMOAND_W_AQ_RL:
		case RISCV_INS_AMOAND_D_AQ_RL:
		case RISCV_INS_AMOOR_W_AQ_RL:
		case RISCV_INS_AMOOR_D_AQ_RL:
		case RISCV_INS_AMOXOR_W_AQ_RL:
		case RISCV_INS_AMOXOR_D_AQ_RL:
		case RISCV_INS_AMOSWAP_W_AQ_RL:
		case RISCV_INS_AMOSWAP_D_AQ_RL:
		case RISCV_INS_AMOMAX_W_AQ_RL:
		case RISCV_INS_AMOMAX_D_AQ_RL:
		case RISCV_INS_AMOMAXU_W_AQ_RL:
		case RISCV_INS_AMOMAXU_D_AQ_RL:
		case RISCV_INS_AMOMIN_W_AQ_RL:
		case RISCV_INS_AMOMIN_D_AQ_RL:
		case RISCV_INS_AMOMINU_W_AQ_RL:
		case RISCV_INS_AMOMINU_D_AQ_RL:
		case RISCV_INS_LR_W_AQ_RL:
		case RISCV_INS_LR_D_AQ_RL:
		case RISCV_INS_SC_W_AQ_RL:
		case RISCV_INS_SC_D_AQ_RL:
			return llvm::AtomicOrdering::SequentiallyConsistent;
		default:
			return llvm::AtomicOrdering::Monotonic;
	}
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::amoAccessType(unsigned id, llvm::IRBuilder<>& irb)
{
	switch (id)
	{
		case RISCV_INS_AMOADD_D:
		case RISCV_INS_AMOADD_D_AQ:
		case RISCV_INS_AMOADD_D_RL:
		case RISCV_INS_AMOADD_D_AQ_RL:
		case RISCV_INS_AMOAND_D:
		case RISCV_INS_AMOAND_D_AQ:
		case RISCV_INS_AMOAND_D_RL:
		case RISCV_INS_AMOAND_D_AQ_RL:
		case RISCV_INS_AMOOR_D:
		case RISCV_INS_AMOOR_D_AQ:
		case RISCV_INS_AMOOR_D_RL:
		case RISCV_INS_AMOOR_D_AQ_RL:
		case RISCV_INS_AMOXOR_D:
		case RISCV_INS_AMOXOR_D_AQ:
		case RISCV_INS_AMOXOR_D_RL:
		case RISCV_INS_AMOXOR_D_AQ_RL:
		case RISCV_INS_AMOSWAP_D:
		case RISCV_INS_AMOSWAP_D_AQ:
		case RISCV_INS_AMOSWAP_D_RL:
		case RISCV_INS_AMOSWAP_D_AQ_RL:
		case RISCV_INS_AMOMAX_D:
		case RISCV_INS_AMOMAX_D_AQ:
		case RISCV_INS_AMOMAX_D_RL:
		case RISCV_INS_AMOMAX_D_AQ_RL:
		case RISCV_INS_AMOMAXU_D:
		case RISCV_INS_AMOMAXU_D_AQ:
		case RISCV_INS_AMOMAXU_D_RL:
		case RISCV_INS_AMOMAXU_D_AQ_RL:
		case RISCV_INS_AMOMIN_D:
		case RISCV_INS_AMOMIN_D_AQ:
		case RISCV_INS_AMOMIN_D_RL:
		case RISCV_INS_AMOMIN_D_AQ_RL:
		case RISCV_INS_AMOMINU_D:
		case RISCV_INS_AMOMINU_D_AQ:
		case RISCV_INS_AMOMINU_D_RL:
		case RISCV_INS_AMOMINU_D_AQ_RL:
		case RISCV_INS_LR_D:
		case RISCV_INS_LR_D_AQ:
		case RISCV_INS_LR_D_RL:
		case RISCV_INS_LR_D_AQ_RL:
		case RISCV_INS_SC_D:
		case RISCV_INS_SC_D_AQ:
		case RISCV_INS_SC_D_RL:
		case RISCV_INS_SC_D_AQ_RL:
			return irb.getInt64Ty();
		default:
			return irb.getInt32Ty();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::definedDivisor(
		llvm::Value* dividend,
		llvm::Value* divisor,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	auto* ty = divisor->getType();
	llvm::Type* elemTy = ty;
	if (auto* vt = llvm::dyn_cast<llvm::VectorType>(ty))
	{
		elemTy = vt->getElementType();
	}
	unsigned bits = llvm::cast<llvm::IntegerType>(elemTy)->getBitWidth();
	auto* one = llvm::Constant::getIntegerValue(ty, llvm::APInt(bits, 1));
	if (isSigned)
	{
		auto* intMin = llvm::Constant::getIntegerValue(ty, llvm::APInt::getSignedMinValue(bits));
		auto* minusOne = llvm::Constant::getIntegerValue(ty, llvm::APInt::getAllOnes(bits));
		auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(dividend, intMin),
				irb.CreateICmpEQ(divisor, minusOne));
		divisor = irb.CreateSelect(overflow, one, divisor);
	}
	return irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, divisor, one);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::fpToInt(
		llvm::Value* v,
		llvm::Type* intTy,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarSizeInBits();
	auto* fpTy = v->getType();
	llvm::Value* loF = nullptr;
	llvm::Value* hiF = nullptr;
	llvm::Value* oob = nullptr;
	if (isSigned)
	{
		loF = llvm::ConstantFP::get(fpTy, -std::ldexp(1.0, static_cast<int>(bits - 1)));
		hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(bits - 1)));
		oob = llvm::ConstantInt::get(intTy, llvm::APInt::getSignedMaxValue(bits));
	}
	else
	{
		loF = llvm::ConstantFP::get(fpTy, 0.0);
		hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(bits)));
		oob = llvm::ConstantInt::get(intTy, llvm::APInt::getMaxValue(bits));
	}
	auto* inRange = irb.CreateAnd(irb.CreateFCmpOGE(v, loF), irb.CreateFCmpOLT(v, hiF));
	auto* safe = irb.CreateSelect(inRange, v, llvm::ConstantFP::get(fpTy, 0.0));
	auto* conv = isSigned ? irb.CreateFPToSI(safe, intTy) : irb.CreateFPToUI(safe, intTy);
	return irb.CreateSelect(inRange, conv, oob);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::fpIntrinsic(
		llvm::IRBuilder<>& irb,
		llvm::Intrinsic::ID id,
		llvm::ArrayRef<llvm::Value*> args)
{
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, id, args.front()->getType());
	return irb.CreateCall(f, args);
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::fpTypeOfInsn(unsigned id, llvm::IRBuilder<>& irb) const
{
	switch (id)
	{
		case RISCV_INS_FMADD_D:
		case RISCV_INS_FMSUB_D:
		case RISCV_INS_FNMADD_D:
		case RISCV_INS_FNMSUB_D:
		case RISCV_INS_FSQRT_D:
		case RISCV_INS_FSGNJ_D:
		case RISCV_INS_FSGNJN_D:
		case RISCV_INS_FSGNJX_D:
		case RISCV_INS_FMIN_D:
		case RISCV_INS_FMAX_D:
		case RISCV_INS_FMV_D_X:
		case RISCV_INS_FMV_X_D:
		case RISCV_INS_FCLASS_D:
		case RISCV_INS_FMINM_D:
		case RISCV_INS_FMAXM_D:
		case RISCV_INS_FROUND_D:
		case RISCV_INS_FROUNDNX_D:
			return irb.getDoubleTy();
		default:
			return irb.getFloatTy();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::asFp(
		llvm::Value* v,
		llvm::Type* ty,
		llvm::IRBuilder<>& irb)
{
	if (v->getType() == ty)
	{
		return v;
	}
	if (v->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
	{
		return irb.CreateFPCast(v, ty);
	}
	return generateTypeConversion(irb, v, ty, eOpConv::FPCAST_OR_BITCAST);
}

uint32_t Capstone2LlvmIrTranslatorRiscv_impl::csrOperandReg(uint16_t encoding) const
{
	switch (encoding)
	{
		case RISCV_SYSREG_FFLAGS: return RISCV_REG_FFLAGS;
		case RISCV_SYSREG_FRM: return RISCV_REG_FRM;
		case RISCV_SYSREG_VL: return RISCV_REG_VL;
		case RISCV_SYSREG_VTYPE: return RISCV_REG_VTYPE;
		case RISCV_SYSREG_VLENB: return RISCV_REG_VLENB;
		case RISCV_SYSREG_VXSAT: return RISCV_REG_VXSAT;
		case RISCV_SYSREG_VXRM: return RISCV_REG_VXRM;
		case RISCV_SYSREG_FCSR:
		case RISCV_SYSREG_CYCLE:
		case RISCV_SYSREG_TIME:
		case RISCV_SYSREG_INSTRET:
		case RISCV_SYSREG_CYCLEH:
		case RISCV_SYSREG_TIMEH:
		case RISCV_SYSREG_INSTRETH:
		case RISCV_SYSREG_SSTATUS:
		case RISCV_SYSREG_SIE:
		case RISCV_SYSREG_STVEC:
		case RISCV_SYSREG_SSCRATCH:
		case RISCV_SYSREG_SEPC:
		case RISCV_SYSREG_SCAUSE:
		case RISCV_SYSREG_STVAL:
		case RISCV_SYSREG_SIP:
		case RISCV_SYSREG_SATP:
		case RISCV_SYSREG_MSTATUS:
		case RISCV_SYSREG_MISA:
		case RISCV_SYSREG_MEDELEG:
		case RISCV_SYSREG_MIDELEG:
		case RISCV_SYSREG_MIE:
		case RISCV_SYSREG_MTVEC:
		case RISCV_SYSREG_MSCRATCH:
		case RISCV_SYSREG_MEPC:
		case RISCV_SYSREG_MCAUSE:
		case RISCV_SYSREG_MTVAL:
		case RISCV_SYSREG_MIP:
		case RISCV_SYSREG_MHARTID:
		case RISCV_SYSREG_MVENDORID:
		case RISCV_SYSREG_MARCHID:
		case RISCV_SYSREG_MIMPID:
		case RISCV_SYSREG_JVT:
			return csrLlvmRegId(encoding);
		default:
			return RISCV_REG_INVALID;
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::loadCsr(uint16_t encoding, llvm::IRBuilder<>& irb)
{
	if (encoding == RISCV_SYSREG_FCSR)
	{
		auto* flags = loadRegister(RISCV_REG_FFLAGS, irb);
		auto* frm = loadRegister(RISCV_REG_FRM, irb);
		auto* ty = getDefaultType();
		flags = irb.CreateZExtOrTrunc(flags, ty);
		frm = irb.CreateZExtOrTrunc(frm, ty);
		auto* fmask = llvm::ConstantInt::get(ty, 0x1f);
		auto* rmask = llvm::ConstantInt::get(ty, 0x7);
		return irb.CreateOr(
				irb.CreateAnd(flags, fmask),
				irb.CreateShl(irb.CreateAnd(frm, rmask), llvm::ConstantInt::get(ty, 5)));
	}
	auto r = csrOperandReg(encoding);
	if (r == RISCV_REG_INVALID || getRegister(r) == nullptr)
	{
		return nullptr;
	}
	return loadRegister(r, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::storeCsr(
		uint16_t encoding,
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	if (encoding == RISCV_SYSREG_FCSR)
	{
		auto* ty = getDefaultType();
		val = irb.CreateZExtOrTrunc(val, ty);
		storeRegister(RISCV_REG_FFLAGS, irb.CreateAnd(val, llvm::ConstantInt::get(ty, 0x1f)), irb);
		storeRegister(
				RISCV_REG_FRM,
				irb.CreateAnd(
						irb.CreateLShr(val, llvm::ConstantInt::get(ty, 5)),
						llvm::ConstantInt::get(ty, 0x7)),
				irb);
		return;
	}
	auto r = csrOperandReg(encoding);
	if (r == RISCV_REG_INVALID || getRegister(r) == nullptr)
	{
		return;
	}
	storeRegister(r, val, irb);
}

/**
 * MUL/MULH/MULHSU/MULHU/MULW, C_MUL.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMul(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	bool word = i->id == RISCV_INS_MULW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}

	bool high = i->id == RISCV_INS_MULH || i->id == RISCV_INS_MULHU || i->id == RISCV_INS_MULHSU;
	unsigned half = op1->getType()->getIntegerBitWidth();
	auto* wide = irb.getIntNTy(half * 2);

	if (i->id == RISCV_INS_MULHU)
	{
		op1 = irb.CreateZExt(op1, wide);
		op2 = irb.CreateZExt(op2, wide);
	}
	else if (i->id == RISCV_INS_MULHSU)
	{
		op1 = irb.CreateSExt(op1, wide);
		op2 = irb.CreateZExt(op2, wide);
	}
	else
	{
		op1 = irb.CreateSExt(op1, wide);
		op2 = irb.CreateSExt(op2, wide);
	}

	auto* mul = irb.CreateMul(op1, op2);
	auto* halfTy = irb.getIntNTy(half);
	llvm::Value* res = high
			? irb.CreateTrunc(irb.CreateLShr(mul, llvm::ConstantInt::get(wide, half)), halfTy)
			: irb.CreateTrunc(mul, halfTy);
	if (word)
	{
		res = widenFromWord(irb, res);
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * DIV/DIVU/REM/REMU and W variants. RISC-V names the zero-divisor and
 * signed-overflow results, so the IR never carries LLVM sdiv/udiv UB.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateDiv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	bool word = i->id == RISCV_INS_DIVW || i->id == RISCV_INS_DIVUW
			|| i->id == RISCV_INS_REMW || i->id == RISCV_INS_REMUW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}

	bool isSigned = i->id == RISCV_INS_DIV || i->id == RISCV_INS_DIVW
			|| i->id == RISCV_INS_REM || i->id == RISCV_INS_REMW;
	bool remainder = i->id == RISCV_INS_REM || i->id == RISCV_INS_REMU
			|| i->id == RISCV_INS_REMW || i->id == RISCV_INS_REMUW;

	auto* ty = op1->getType();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* divZero = irb.CreateICmpEQ(op2, zero);
	auto* safe = definedDivisor(op1, op2, isSigned, irb);

	llvm::Value* arith = nullptr;
	llvm::Value* dzRes = nullptr;
	if (remainder)
	{
		arith = isSigned ? irb.CreateSRem(op1, safe) : irb.CreateURem(op1, safe);
		dzRes = op1;
	}
	else if (isSigned)
	{
		arith = irb.CreateSDiv(op1, safe);
		dzRes = llvm::ConstantInt::getSigned(ty, -1);
	}
	else
	{
		arith = irb.CreateUDiv(op1, safe);
		dzRes = llvm::ConstantInt::getAllOnesValue(ty);
	}

	llvm::Value* res = irb.CreateSelect(divZero, dzRes, arith);
	if (isSigned)
	{
		unsigned bits = llvm::cast<llvm::IntegerType>(ty)->getBitWidth();
		auto* intMin = llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(bits));
		auto* minusOne = llvm::ConstantInt::getSigned(ty, -1);
		auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(op1, intMin),
				irb.CreateICmpEQ(op2, minusOne));
		auto* ovRes = remainder ? llvm::ConstantInt::get(ty, 0) : intMin;
		res = irb.CreateSelect(overflow, ovRes, res);
	}

	if (word)
	{
		res = widenFromWord(irb, res);
	}
	storeOp(ri->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFpArith(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	op1 = loadOp(ri->operands[1], irb);
	op2 = loadOp(ri->operands[2], irb);
	if (op1->getType() != op2->getType() && op2->getType()->isFloatingPointTy())
	{
		op1 = irb.CreateFPCast(op1, op2->getType());
	}
	else if (op1->getType() != op2->getType() && op1->getType()->isFloatingPointTy())
	{
		op2 = irb.CreateFPCast(op2, op1->getType());
	}

	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FADD_S:
		case RISCV_INS_FADD_D:
			res = irb.CreateFAdd(op1, op2);
			break;
		case RISCV_INS_FSUB_S:
		case RISCV_INS_FSUB_D:
			res = irb.CreateFSub(op1, op2);
			break;
		case RISCV_INS_FMUL_S:
		case RISCV_INS_FMUL_D:
			res = irb.CreateFMul(op1, op2);
			break;
		case RISCV_INS_FDIV_S:
		case RISCV_INS_FDIV_D:
			res = irb.CreateFDiv(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpArith().");
	}
	storeOp(ri->operands[0], res, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFpLoad(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FLW:
		case RISCV_INS_C_FLW:
		case RISCV_INS_C_FLWSP:
			ty = irb.getFloatTy();
			break;
		case RISCV_INS_FLD:
		case RISCV_INS_C_FLD:
		case RISCV_INS_C_FLDSP:
			ty = irb.getDoubleTy();
			break;
		case RISCV_INS_FLH:
			ty = irb.getHalfTy();
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpLoad().");
	}

	if (ri->operands[1].type == RISCV_OP_MEM
			|| (ri->op_count == 2 && ri->operands[1].type != RISCV_OP_REG))
	{
		op1 = loadOp(ri->operands[1], irb, ty);
	}
	else if (ri->op_count >= 3)
	{
		auto* base = loadOp(ri->operands[1], irb);
		auto* off = loadOp(ri->operands[2], irb);
		off = irb.CreateSExtOrTrunc(off, base->getType());
		op1 = loadIntPtr(irb, irb.CreateAdd(base, off), ty);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	storeOp(ri->operands[0], op1, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFpStore(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FSW:
		case RISCV_INS_C_FSW:
		case RISCV_INS_C_FSWSP:
			ty = irb.getFloatTy();
			break;
		case RISCV_INS_FSD:
		case RISCV_INS_C_FSD:
		case RISCV_INS_C_FSDSP:
			ty = irb.getDoubleTy();
			break;
		case RISCV_INS_FSH:
			ty = irb.getHalfTy();
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpStore().");
	}

	op0 = loadOp(ri->operands[0], irb);
	if (op0->getType() != ty)
	{
		if (op0->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
		{
			op0 = irb.CreateFPCast(op0, ty);
		}
		else if (ty->isFloatingPointTy())
		{
			op0 = generateTypeConversion(irb, op0, ty, eOpConv::FPCAST_OR_BITCAST);
		}
		else
		{
			op0 = irb.CreateBitCast(op0, ty);
		}
	}

	if (ri->operands[1].type == RISCV_OP_MEM
			|| (ri->op_count == 2 && ri->operands[1].type != RISCV_OP_REG))
	{
		storeOp(ri->operands[1], op0, irb);
	}
	else if (ri->op_count >= 3)
	{
		auto* base = loadOp(ri->operands[1], irb);
		auto* off = loadOp(ri->operands[2], irb);
		off = irb.CreateSExtOrTrunc(off, base->getType());
		storeIntPtr(irb, op0, irb.CreateAdd(base, off), ty);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	llvm::Value* res = nullptr;

	switch (i->id)
	{
		case RISCV_INS_FCVT_S_D:
			res = irb.CreateFPTrunc(op1, irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_D_S:
			res = irb.CreateFPExt(op1, irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_S_W:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt32Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_S_WU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt32Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_D_W:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt32Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_D_WU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt32Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_S_L:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt64Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_S_LU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt64Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_D_L:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt64Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_D_LU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt64Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_W_S:
		case RISCV_INS_FCVT_W_D:
			res = widenFromWord(irb, fpToInt(op1, irb.getInt32Ty(), /*isSigned=*/true, irb));
			break;
		case RISCV_INS_FCVT_WU_S:
		case RISCV_INS_FCVT_WU_D:
			res = irb.CreateZExt(fpToInt(op1, irb.getInt32Ty(), /*isSigned=*/false, irb), getDefaultType());
			break;
		case RISCV_INS_FCVT_L_S:
		case RISCV_INS_FCVT_L_D:
			res = fpToInt(op1, irb.getInt64Ty(), /*isSigned=*/true, irb);
			break;
		case RISCV_INS_FCVT_LU_S:
		case RISCV_INS_FCVT_LU_D:
			res = fpToInt(op1, irb.getInt64Ty(), /*isSigned=*/false, irb);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFcvt().");
	}
	storeOp(ri->operands[0], res, irb, res->getType()->isFloatingPointTy()
			? eOpConv::FPCAST_OR_BITCAST
			: eOpConv::SEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	op1 = loadOp(ri->operands[1], irb);
	op2 = loadOp(ri->operands[2], irb);
	if (op1->getType() != op2->getType() && op1->getType()->isFloatingPointTy())
	{
		op2 = irb.CreateFPCast(op2, op1->getType());
	}

	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FEQ_S:
		case RISCV_INS_FEQ_D:
			cmp = irb.CreateFCmpOEQ(op1, op2);
			break;
		case RISCV_INS_FLT_S:
		case RISCV_INS_FLT_D:
		// Zfa FLTQ/FLEQ differ from FLT/FLE only in not raising invalid
		// on a quiet NaN, which the flags model does not carry.
		case RISCV_INS_FLTQ_S:
		case RISCV_INS_FLTQ_D:
			cmp = irb.CreateFCmpOLT(op1, op2);
			break;
		case RISCV_INS_FLE_S:
		case RISCV_INS_FLE_D:
		case RISCV_INS_FLEQ_S:
		case RISCV_INS_FLEQ_D:
			cmp = irb.CreateFCmpOLE(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFcmp().");
	}
	storeOp(ri->operands[0], irb.CreateZExt(cmp, getDefaultType()), irb);
}

/**
 * FMADD/FMSUB/FNMADD/FNMSUB S/D: rd = ±(rs1 * rs2) ± rs3.
 * Capstone operands follow assembly order: rd, rs1, rs2, rs3 [, rm].
 * rm lives in cs_riscv.rounding_mode and is ignored, same as FADD.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateFpFma(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 4)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* rs1 = asFp(loadOp(ri->operands[1], irb), ty, irb);
	auto* rs2 = asFp(loadOp(ri->operands[2], irb), ty, irb);
	auto* rs3 = asFp(loadOp(ri->operands[3], irb), ty, irb);

	llvm::Value* a = rs1;
	llvm::Value* c = rs3;
	switch (i->id)
	{
		case RISCV_INS_FMADD_S:
		case RISCV_INS_FMADD_D:
			break;
		case RISCV_INS_FMSUB_S:
		case RISCV_INS_FMSUB_D:
			c = irb.CreateFNeg(rs3);
			break;
		case RISCV_INS_FNMSUB_S:
		case RISCV_INS_FNMSUB_D:
			a = irb.CreateFNeg(rs1);
			break;
		case RISCV_INS_FNMADD_S:
		case RISCV_INS_FNMADD_D:
			a = irb.CreateFNeg(rs1);
			c = irb.CreateFNeg(rs3);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpFma().");
	}

	storeOp(ri->operands[0], fpIntrinsic(irb, llvm::Intrinsic::fma, {a, rs2, c}), irb,
			eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFsqrt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* src = asFp(loadOp(ri->operands[1], irb), ty, irb);
	storeOp(ri->operands[0], fpIntrinsic(irb, llvm::Intrinsic::sqrt, {src}), irb,
			eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFsgnj(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* rs1 = asFp(loadOp(ri->operands[1], irb), ty, irb);
	auto* rs2 = asFp(loadOp(ri->operands[2], irb), ty, irb);
	unsigned bits = ty->isDoubleTy() ? 64u : 32u;
	auto* intTy = irb.getIntNTy(bits);
	auto* ia = irb.CreateBitCast(rs1, intTy);
	auto* ib = irb.CreateBitCast(rs2, intTy);
	auto* signMask = llvm::ConstantInt::get(intTy, llvm::APInt::getSignMask(bits));
	auto* magMask = llvm::ConstantInt::get(intTy, ~llvm::APInt::getSignMask(bits));

	llvm::Value* bitsOut = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FSGNJ_S:
		case RISCV_INS_FSGNJ_D:
			bitsOut = irb.CreateOr(irb.CreateAnd(ia, magMask), irb.CreateAnd(ib, signMask));
			break;
		case RISCV_INS_FSGNJN_S:
		case RISCV_INS_FSGNJN_D:
			bitsOut = irb.CreateOr(
					irb.CreateAnd(ia, magMask),
					irb.CreateAnd(irb.CreateNot(ib), signMask));
			break;
		case RISCV_INS_FSGNJX_S:
		case RISCV_INS_FSGNJX_D:
			bitsOut = irb.CreateXor(ia, irb.CreateAnd(ib, signMask));
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFsgnj().");
	}
	storeOp(ri->operands[0], irb.CreateBitCast(bitsOut, ty), irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFminMax(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* rs1 = asFp(loadOp(ri->operands[1], irb), ty, irb);
	auto* rs2 = asFp(loadOp(ri->operands[2], irb), ty, irb);
	llvm::Intrinsic::ID iid = llvm::Intrinsic::minnum;
	bool nanPropagating = false;
	switch (i->id)
	{
		case RISCV_INS_FMIN_S:
		case RISCV_INS_FMIN_D:
			iid = llvm::Intrinsic::minnum;
			break;
		case RISCV_INS_FMAX_S:
		case RISCV_INS_FMAX_D:
			iid = llvm::Intrinsic::maxnum;
			break;
		case RISCV_INS_FMINM_S:
		case RISCV_INS_FMINM_D:
			iid = llvm::Intrinsic::minnum;
			nanPropagating = true;
			break;
		case RISCV_INS_FMAXM_S:
		case RISCV_INS_FMAXM_D:
			iid = llvm::Intrinsic::maxnum;
			nanPropagating = true;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFminMax().");
	}
	llvm::Value* res = fpIntrinsic(irb, iid, {rs1, rs2});
	if (nanPropagating)
	{
		// Zfa FMINM/FMAXM return the canonical NaN when either input is a
		// NaN, where FMIN/FMAX return the other operand. llvm.minimum would
		// say that directly, but llvmir-emul cannot evaluate it.
		auto* anyNan = irb.CreateFCmpUNO(rs1, rs2);
		res = irb.CreateSelect(anyNan, llvm::ConstantFP::getNaN(ty), res);
	}
	storeOp(ri->operands[0], res, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * FCLASS_S / FCLASS_D: 10-bit mask in rd (unprivileged spec).
 * bit0 -inf, 1 neg normal, 2 neg subnormal, 3 -0, 4 +0, 5 pos subnormal,
 * 6 pos normal, 7 +inf, 8 sNaN, 9 qNaN.
 *
 * LLVM 23 has Intrinsic::is_fpclass, but llvmir-emul does not lower it, so
 * this is bitcast + icmp/select. Quiet vs signaling uses the fraction MSB.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateFclass(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* src = asFp(loadOp(ri->operands[1], irb), ty, irb);
	unsigned nbits = ty->isDoubleTy() ? 64u : 32u;
	unsigned fracBits = ty->isDoubleTy() ? 52u : 23u;
	auto* intTy = irb.getIntNTy(nbits);
	auto* bits = irb.CreateBitCast(src, intTy);
	auto* zero = llvm::ConstantInt::get(intTy, 0);
	auto* signMask = llvm::ConstantInt::get(intTy, llvm::APInt::getSignMask(nbits));
	auto* fracMask = llvm::ConstantInt::get(intTy, llvm::APInt::getLowBitsSet(nbits, fracBits));
	auto* expMask = llvm::ConstantInt::get(
			intTy,
			llvm::APInt::getBitsSet(nbits, fracBits, nbits - 1));
	auto* qnanBit = llvm::ConstantInt::get(
			intTy,
			llvm::APInt::getOneBitSet(nbits, fracBits - 1));

	auto* sign = irb.CreateAnd(bits, signMask);
	auto* exp = irb.CreateAnd(bits, expMask);
	auto* frac = irb.CreateAnd(bits, fracMask);
	auto* qbit = irb.CreateAnd(bits, qnanBit);

	auto* isNeg = irb.CreateICmpNE(sign, zero);
	auto* isPos = irb.CreateICmpEQ(sign, zero);
	auto* exp0 = irb.CreateICmpEQ(exp, zero);
	auto* expMax = irb.CreateICmpEQ(exp, expMask);
	auto* frac0 = irb.CreateICmpEQ(frac, zero);
	auto* fracNz = irb.CreateICmpNE(frac, zero);
	auto* expNorm = irb.CreateAnd(irb.CreateICmpNE(exp, zero), irb.CreateICmpNE(exp, expMask));
	auto* isQuiet = irb.CreateICmpNE(qbit, zero);
	auto* isSig = irb.CreateICmpEQ(qbit, zero);

	auto* dstTy = getDefaultType();
	auto* z = llvm::ConstantInt::get(dstTy, 0);
	auto classBit = [&](llvm::Value* cond, unsigned b) -> llvm::Value* {
		return irb.CreateSelect(cond, llvm::ConstantInt::get(dstTy, 1ull << b), z);
	};

	llvm::Value* res = classBit(irb.CreateAnd(isNeg, irb.CreateAnd(expMax, frac0)), 0);
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, expNorm), 1));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, irb.CreateAnd(exp0, fracNz)), 2));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, irb.CreateAnd(exp0, frac0)), 3));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(exp0, frac0)), 4));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(exp0, fracNz)), 5));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, expNorm), 6));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(expMax, frac0)), 7));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(expMax, irb.CreateAnd(fracNz, isSig)), 8));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(expMax, irb.CreateAnd(fracNz, isQuiet)), 9));
	storeOp(ri->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFmv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = loadOp(ri->operands[1], irb);
	switch (i->id)
	{
		case RISCV_INS_FMV_W_X:
		{
			if (!op1->getType()->isIntegerTy())
			{
				op1 = irb.CreateBitCast(
						op1,
						irb.getIntNTy(op1->getType()->getPrimitiveSizeInBits()));
			}
			auto* bits = irb.CreateTrunc(op1, irb.getInt32Ty());
			storeOp(ri->operands[0], irb.CreateBitCast(bits, irb.getFloatTy()), irb,
					eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case RISCV_INS_FMV_X_W:
		{
			if (op1->getType()->isFloatingPointTy())
			{
				unsigned n = op1->getType()->getPrimitiveSizeInBits();
				op1 = irb.CreateBitCast(op1, irb.getIntNTy(n));
			}
			auto* bits = irb.CreateTrunc(op1, irb.getInt32Ty());
			storeOp(ri->operands[0], bits, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
			return;
		}
		case RISCV_INS_FMV_D_X:
		{
			if (!op1->getType()->isIntegerTy())
			{
				op1 = irb.CreateBitCast(
						op1,
						irb.getIntNTy(op1->getType()->getPrimitiveSizeInBits()));
			}
			auto* bits = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
			storeOp(ri->operands[0], irb.CreateBitCast(bits, irb.getDoubleTy()), irb,
					eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case RISCV_INS_FMV_X_D:
		{
			if (op1->getType()->isFloatingPointTy())
			{
				unsigned n = op1->getType()->getPrimitiveSizeInBits();
				op1 = irb.CreateBitCast(op1, irb.getIntNTy(n));
			}
			auto* bits = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
			storeOp(ri->operands[0], bits, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
			return;
		}
		default:
			throw GenericError("Unhandled insn ID in translateFmv().");
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateAmo(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ri, irb);

	llvm::AtomicRMWInst::BinOp op;
	switch (i->id)
	{
		case RISCV_INS_AMOADD_W:
		case RISCV_INS_AMOADD_W_AQ:
		case RISCV_INS_AMOADD_W_RL:
		case RISCV_INS_AMOADD_W_AQ_RL:
		case RISCV_INS_AMOADD_D:
		case RISCV_INS_AMOADD_D_AQ:
		case RISCV_INS_AMOADD_D_RL:
		case RISCV_INS_AMOADD_D_AQ_RL:
			op = llvm::AtomicRMWInst::Add;
			break;
		case RISCV_INS_AMOAND_W:
		case RISCV_INS_AMOAND_W_AQ:
		case RISCV_INS_AMOAND_W_RL:
		case RISCV_INS_AMOAND_W_AQ_RL:
		case RISCV_INS_AMOAND_D:
		case RISCV_INS_AMOAND_D_AQ:
		case RISCV_INS_AMOAND_D_RL:
		case RISCV_INS_AMOAND_D_AQ_RL:
			op = llvm::AtomicRMWInst::And;
			break;
		case RISCV_INS_AMOOR_W:
		case RISCV_INS_AMOOR_W_AQ:
		case RISCV_INS_AMOOR_W_RL:
		case RISCV_INS_AMOOR_W_AQ_RL:
		case RISCV_INS_AMOOR_D:
		case RISCV_INS_AMOOR_D_AQ:
		case RISCV_INS_AMOOR_D_RL:
		case RISCV_INS_AMOOR_D_AQ_RL:
			op = llvm::AtomicRMWInst::Or;
			break;
		case RISCV_INS_AMOXOR_W:
		case RISCV_INS_AMOXOR_W_AQ:
		case RISCV_INS_AMOXOR_W_RL:
		case RISCV_INS_AMOXOR_W_AQ_RL:
		case RISCV_INS_AMOXOR_D:
		case RISCV_INS_AMOXOR_D_AQ:
		case RISCV_INS_AMOXOR_D_RL:
		case RISCV_INS_AMOXOR_D_AQ_RL:
			op = llvm::AtomicRMWInst::Xor;
			break;
		case RISCV_INS_AMOSWAP_W:
		case RISCV_INS_AMOSWAP_W_AQ:
		case RISCV_INS_AMOSWAP_W_RL:
		case RISCV_INS_AMOSWAP_W_AQ_RL:
		case RISCV_INS_AMOSWAP_D:
		case RISCV_INS_AMOSWAP_D_AQ:
		case RISCV_INS_AMOSWAP_D_RL:
		case RISCV_INS_AMOSWAP_D_AQ_RL:
			op = llvm::AtomicRMWInst::Xchg;
			break;
		case RISCV_INS_AMOMAX_W:
		case RISCV_INS_AMOMAX_W_AQ:
		case RISCV_INS_AMOMAX_W_RL:
		case RISCV_INS_AMOMAX_W_AQ_RL:
		case RISCV_INS_AMOMAX_D:
		case RISCV_INS_AMOMAX_D_AQ:
		case RISCV_INS_AMOMAX_D_RL:
		case RISCV_INS_AMOMAX_D_AQ_RL:
			op = llvm::AtomicRMWInst::Max;
			break;
		case RISCV_INS_AMOMAXU_W:
		case RISCV_INS_AMOMAXU_W_AQ:
		case RISCV_INS_AMOMAXU_W_RL:
		case RISCV_INS_AMOMAXU_W_AQ_RL:
		case RISCV_INS_AMOMAXU_D:
		case RISCV_INS_AMOMAXU_D_AQ:
		case RISCV_INS_AMOMAXU_D_RL:
		case RISCV_INS_AMOMAXU_D_AQ_RL:
			op = llvm::AtomicRMWInst::UMax;
			break;
		case RISCV_INS_AMOMIN_W:
		case RISCV_INS_AMOMIN_W_AQ:
		case RISCV_INS_AMOMIN_W_RL:
		case RISCV_INS_AMOMIN_W_AQ_RL:
		case RISCV_INS_AMOMIN_D:
		case RISCV_INS_AMOMIN_D_AQ:
		case RISCV_INS_AMOMIN_D_RL:
		case RISCV_INS_AMOMIN_D_AQ_RL:
			op = llvm::AtomicRMWInst::Min;
			break;
		case RISCV_INS_AMOMINU_W:
		case RISCV_INS_AMOMINU_W_AQ:
		case RISCV_INS_AMOMINU_W_RL:
		case RISCV_INS_AMOMINU_W_AQ_RL:
		case RISCV_INS_AMOMINU_D:
		case RISCV_INS_AMOMINU_D_AQ:
		case RISCV_INS_AMOMINU_D_RL:
		case RISCV_INS_AMOMINU_D_AQ_RL:
			op = llvm::AtomicRMWInst::UMin;
			break;
		default:
			translatePseudoAsmGeneric(i, ri, irb);
			return;
	}

	auto* elem = amoAccessType(i->id, irb);
	auto* addr = amoAddress(ri->operands[1], irb);
	auto* val = loadOp(ri->operands[2], irb);
	val = generateTypeConversion(irb, val, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ptr = intToPtr(irb, addr, elem);
	auto* old = irb.CreateAtomicRMW(op, ptr, val, llvm::MaybeAlign(), amoOrdering(i->id));
	attachPointeeType(old, elem);
	storeOp(ri->operands[0], old, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * Zacas AMOCAS.W/D: compare-and-swap. rd holds the expected value on entry
 * and the old memory value on exit; rs2 is what gets stored on a match.
 * Capstone orders the operands rd, (rs1), rs2, as for the other AMOs.
 * AMOCAS.Q works on register pairs and stays a named call.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateAmocas(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ri, irb);

	llvm::Type* elem = nullptr;
	auto success = llvm::AtomicOrdering::Monotonic;
	auto failure = llvm::AtomicOrdering::Monotonic;
	switch (i->id)
	{
		case RISCV_INS_AMOCAS_W: elem = irb.getInt32Ty(); break;
		case RISCV_INS_AMOCAS_D: elem = irb.getInt64Ty(); break;
		case RISCV_INS_AMOCAS_W_AQ:
		case RISCV_INS_AMOCAS_D_AQ:
			elem = i->id == RISCV_INS_AMOCAS_W_AQ ? irb.getInt32Ty() : irb.getInt64Ty();
			success = failure = llvm::AtomicOrdering::Acquire;
			break;
		case RISCV_INS_AMOCAS_W_RL:
		case RISCV_INS_AMOCAS_D_RL:
			elem = i->id == RISCV_INS_AMOCAS_W_RL ? irb.getInt32Ty() : irb.getInt64Ty();
			success = llvm::AtomicOrdering::Release;
			break;
		case RISCV_INS_AMOCAS_W_AQRL:
		case RISCV_INS_AMOCAS_D_AQRL:
			elem = i->id == RISCV_INS_AMOCAS_W_AQRL ? irb.getInt32Ty() : irb.getInt64Ty();
			success = failure = llvm::AtomicOrdering::SequentiallyConsistent;
			break;
		default:
			translatePseudoAsmGeneric(i, ri, irb);
			return;
	}
	if (elem->getIntegerBitWidth() == 64 && !isXlen64())
	{
		// RV32 AMOCAS.D takes register pairs, like AMOCAS.Q on RV64.
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* addr = amoAddress(ri->operands[1], irb);
	auto* expected = generateTypeConversion(irb, loadOp(ri->operands[0], irb), elem,
			eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* desired = generateTypeConversion(irb, loadOp(ri->operands[2], irb), elem,
			eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ptr = intToPtr(irb, addr, elem);
	auto* cx = irb.CreateAtomicCmpXchg(ptr, expected, desired, llvm::MaybeAlign(), success, failure);
	attachPointeeType(cx, elem);
	storeOp(ri->operands[0], irb.CreateExtractValue(cx, 0), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * LR.W/D: atomic load. No exclusive-monitor model (same as ARM LDXR).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	auto* elem = amoAccessType(i->id, irb);
	auto* addr = amoAddress(ri->operands[1], irb);
	auto* ld = loadIntPtr(irb, addr, elem);
	ld->setAtomic(amoOrdering(i->id));
	storeOp(ri->operands[0], ld, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * SC.W/D: atomic store + status 0. No exclusive-monitor model (same as ARM STXR).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateSc(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ri, irb);
	auto* elem = amoAccessType(i->id, irb);
	auto* addr = amoAddress(ri->operands[1], irb);
	auto* val = loadOp(ri->operands[2], irb);
	val = generateTypeConversion(irb, val, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* st = storeIntPtr(irb, val, addr, elem);
	st->setAtomic(amoOrdering(i->id));
	storeOp(ri->operands[0], llvm::ConstantInt::get(getDefaultType(), 0), irb);
}

/**
 * CSRRW/CSRRS/CSRRC and immediates. Capstone SYSREG encodings only; unknown
 * encodings stay a named __asm_* call rather than inventing a CSR number.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateCsr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	uint16_t encoding = 0;
	bool haveCsr = false;
	int rdOp = -1;
	int srcOp = -1;
	for (int n = 0; n < ri->op_count; ++n)
	{
		auto& op = ri->operands[n];
		if (op.type == RISCV_OP_CSR)
		{
			encoding = op.csr;
			haveCsr = true;
		}
		else if (op.type == RISCV_OP_REG
				&& (op.reg == RISCV_REG_FFLAGS
						|| op.reg == RISCV_REG_FRM
						|| csrOperandReg(static_cast<uint16_t>(op.reg)) != RISCV_REG_INVALID))
		{
			encoding = static_cast<uint16_t>(op.reg);
			haveCsr = true;
		}
		else if (op.type == RISCV_OP_REG)
		{
			if (rdOp < 0)
			{
				rdOp = n;
			}
			else
			{
				srcOp = n;
			}
		}
		else if (op.type == RISCV_OP_IMM)
		{
			if (!haveCsr && n != 0)
			{
				encoding = static_cast<uint16_t>(op.imm);
				haveCsr = true;
			}
			else
			{
				srcOp = n;
			}
		}
	}

	uint64_t alias = i->is_alias ? i->alias_id : i->id;
	if (!haveCsr)
	{
		switch (alias)
		{
			case RISCV_INS_ALIAS_FRFLAGS:
			case RISCV_INS_ALIAS_FSFLAGS:
			case RISCV_INS_ALIAS_FSFLAGSI:
				encoding = RISCV_SYSREG_FFLAGS;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_FRRM:
			case RISCV_INS_ALIAS_FSRM:
			case RISCV_INS_ALIAS_FSRMI:
				encoding = RISCV_SYSREG_FRM;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_FRCSR:
			case RISCV_INS_ALIAS_FSCSR:
				encoding = RISCV_SYSREG_FCSR;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDCYCLE:
				encoding = RISCV_SYSREG_CYCLE;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDTIME:
				encoding = RISCV_SYSREG_TIME;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDINSTRET:
				encoding = RISCV_SYSREG_INSTRET;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDCYCLEH:
				encoding = RISCV_SYSREG_CYCLEH;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDTIMEH:
				encoding = RISCV_SYSREG_TIMEH;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDINSTRETH:
				encoding = RISCV_SYSREG_INSTRETH;
				haveCsr = true;
				break;
			default:
				break;
		}
	}

	if (!haveCsr)
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	if (csrOperandReg(encoding) == RISCV_REG_INVALID && encoding != RISCV_SYSREG_FCSR)
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	llvm::Value* src = nullptr;
	if (srcOp >= 0)
	{
		src = loadOp(ri->operands[srcOp], irb);
	}
	else
	{
		src = llvm::ConstantInt::get(getDefaultType(), 0);
	}
	src = irb.CreateZExtOrTrunc(src, getDefaultType());

	bool isImm = i->id == RISCV_INS_CSRRWI || i->id == RISCV_INS_CSRRSI || i->id == RISCV_INS_CSRRCI
			|| i->id == RISCV_INS_ALIAS_CSRWI || i->id == RISCV_INS_ALIAS_CSRSI
			|| i->id == RISCV_INS_ALIAS_CSRCI
			|| alias == RISCV_INS_ALIAS_CSRWI || alias == RISCV_INS_ALIAS_CSRSI
			|| alias == RISCV_INS_ALIAS_CSRCI || alias == RISCV_INS_ALIAS_FSFLAGSI
			|| alias == RISCV_INS_ALIAS_FSRMI;
	bool isWrite = i->id == RISCV_INS_CSRRW || i->id == RISCV_INS_CSRRWI
			|| i->id == RISCV_INS_ALIAS_CSRW || i->id == RISCV_INS_ALIAS_CSRWI
			|| i->id == RISCV_INS_ALIAS_FSFLAGS || i->id == RISCV_INS_ALIAS_FSCSR
			|| i->id == RISCV_INS_ALIAS_FSRM || i->id == RISCV_INS_ALIAS_FSFLAGSI
			|| i->id == RISCV_INS_ALIAS_FSRMI
			|| alias == RISCV_INS_ALIAS_CSRW || alias == RISCV_INS_ALIAS_CSRWI
			|| alias == RISCV_INS_ALIAS_FSFLAGS || alias == RISCV_INS_ALIAS_FSCSR
			|| alias == RISCV_INS_ALIAS_FSRM || alias == RISCV_INS_ALIAS_FSFLAGSI
			|| alias == RISCV_INS_ALIAS_FSRMI;
	bool isSet = i->id == RISCV_INS_CSRRS || i->id == RISCV_INS_CSRRSI
			|| i->id == RISCV_INS_ALIAS_CSRS || i->id == RISCV_INS_ALIAS_CSRSI
			|| i->id == RISCV_INS_ALIAS_CSRR || i->id == RISCV_INS_ALIAS_FRFLAGS
			|| i->id == RISCV_INS_ALIAS_FRRM || i->id == RISCV_INS_ALIAS_FRCSR
			|| i->id == RISCV_INS_ALIAS_RDCYCLE || i->id == RISCV_INS_ALIAS_RDTIME
			|| i->id == RISCV_INS_ALIAS_RDINSTRET || i->id == RISCV_INS_ALIAS_RDCYCLEH
			|| i->id == RISCV_INS_ALIAS_RDTIMEH || i->id == RISCV_INS_ALIAS_RDINSTRETH
			|| alias == RISCV_INS_ALIAS_CSRS || alias == RISCV_INS_ALIAS_CSRSI
			|| alias == RISCV_INS_ALIAS_CSRR || alias == RISCV_INS_ALIAS_FRFLAGS
			|| alias == RISCV_INS_ALIAS_FRRM || alias == RISCV_INS_ALIAS_FRCSR
			|| alias == RISCV_INS_ALIAS_RDCYCLE || alias == RISCV_INS_ALIAS_RDTIME
			|| alias == RISCV_INS_ALIAS_RDINSTRET || alias == RISCV_INS_ALIAS_RDCYCLEH
			|| alias == RISCV_INS_ALIAS_RDTIMEH || alias == RISCV_INS_ALIAS_RDINSTRETH;
	bool isClear = i->id == RISCV_INS_CSRRC || i->id == RISCV_INS_CSRRCI
			|| i->id == RISCV_INS_ALIAS_CSRC || i->id == RISCV_INS_ALIAS_CSRCI
			|| alias == RISCV_INS_ALIAS_CSRC || alias == RISCV_INS_ALIAS_CSRCI;

	if (!isWrite && !isSet && !isClear)
	{
		isSet = true;
	}

	bool srcIsZero = false;
	if (srcOp < 0)
	{
		srcIsZero = true;
	}
	else if (isImm || ri->operands[srcOp].type == RISCV_OP_IMM)
	{
		srcIsZero = llvm::isa<llvm::ConstantInt>(src)
				&& llvm::cast<llvm::ConstantInt>(src)->isZero();
	}
	else if (ri->operands[srcOp].type == RISCV_OP_REG)
	{
		srcIsZero = ri->operands[srcOp].reg == RISCV_REG_X0;
	}

	bool rdIsZero = rdOp >= 0
			&& ri->operands[rdOp].type == RISCV_OP_REG
			&& ri->operands[rdOp].reg == RISCV_REG_X0;
	if (rdOp < 0)
	{
		rdIsZero = true;
	}

	llvm::Value* old = nullptr;
	if (!isWrite || !rdIsZero)
	{
		old = loadCsr(encoding, irb);
		if (old == nullptr)
		{
			translatePseudoAsmGeneric(i, ri, irb);
			return;
		}
		old = irb.CreateZExtOrTrunc(old, getDefaultType());
		if (rdOp >= 0)
		{
			storeOp(ri->operands[rdOp], old, irb);
		}
	}

	if (isWrite)
	{
		storeCsr(encoding, src, irb);
	}
	else if ((isSet || isClear) && !srcIsZero)
	{
		if (old == nullptr)
		{
			old = loadCsr(encoding, irb);
			if (old == nullptr)
			{
				translatePseudoAsmGeneric(i, ri, irb);
				return;
			}
			old = irb.CreateZExtOrTrunc(old, getDefaultType());
		}
		auto* next = isSet ? irb.CreateOr(old, src) : irb.CreateAnd(old, irb.CreateNot(src));
		storeCsr(encoding, next, irb);
	}
}

namespace {

llvm::Value* rotateLeft(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* amt)
{
	auto* ty = val->getType();
	unsigned w = ty->getIntegerBitWidth();
	amt = irb.CreateZExtOrTrunc(amt, ty);
	amt = irb.CreateAnd(amt, llvm::ConstantInt::get(ty, w - 1));
	auto* left = irb.CreateShl(val, amt);
	auto* ramt = irb.CreateAnd(
			irb.CreateSub(llvm::ConstantInt::get(ty, 0), amt),
			llvm::ConstantInt::get(ty, w - 1));
	return irb.CreateOr(left, irb.CreateLShr(val, ramt));
}

llvm::Value* rotateRight(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* amt)
{
	auto* ty = val->getType();
	unsigned w = ty->getIntegerBitWidth();
	amt = irb.CreateZExtOrTrunc(amt, ty);
	amt = irb.CreateAnd(amt, llvm::ConstantInt::get(ty, w - 1));
	auto* right = irb.CreateLShr(val, amt);
	auto* lamt = irb.CreateAnd(
			irb.CreateSub(llvm::ConstantInt::get(ty, 0), amt),
			llvm::ConstantInt::get(ty, w - 1));
	return irb.CreateOr(right, irb.CreateShl(val, lamt));
}

} // namespace

/**
 * Zbb ANDN/ORN/XNOR. RISC-V complements rs2: rd = rs1 & ~rs2 (ANDN),
 * rs1 | ~rs2 (ORN), rs1 ^ ~rs2 (XNOR). x86 BMI ANDN is the other way
 * around (~rs1 & rs2). Zcb C.NOT is rd = ~rd (unary compressed REG).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateAndn(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (i->id == RISCV_INS_C_NOT)
	{
		if (ri->op_count < 1)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, ri, irb);
			return;
		}
		unsigned src = ri->op_count >= 2 ? 1u : 0u;
		op1 = loadOp(ri->operands[src], irb);
		storeOp(ri->operands[0], irb.CreateNot(op1), irb);
		return;
	}

	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* n2 = irb.CreateNot(op2);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_ANDN:
			res = irb.CreateAnd(op1, n2);
			break;
		case RISCV_INS_ORN:
			res = irb.CreateOr(op1, n2);
			break;
		case RISCV_INS_XNOR:
			res = irb.CreateXor(op1, n2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateAndn().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbb CLZ/CTZ/CPOP and W variants. llvm.ctlz/cttz with is_zero_poison=false
 * (zero input is defined: XLEN, or 32 for W). W forms use the low 32 bits
 * then sign-extend, same as translateOp32.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBitCount(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = loadOp(ri->operands[1], irb);
	bool word = i->id == RISCV_INS_CLZW
			|| i->id == RISCV_INS_CTZW
			|| i->id == RISCV_INS_CPOPW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
	}
	else
	{
		op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());
	}

	llvm::Intrinsic::ID iid = llvm::Intrinsic::ctlz;
	bool pop = false;
	switch (i->id)
	{
		case RISCV_INS_CLZ:
		case RISCV_INS_CLZW:
			iid = llvm::Intrinsic::ctlz;
			break;
		case RISCV_INS_CTZ:
		case RISCV_INS_CTZW:
			iid = llvm::Intrinsic::cttz;
			break;
		case RISCV_INS_CPOP:
		case RISCV_INS_CPOPW:
			iid = llvm::Intrinsic::ctpop;
			pop = true;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateBitCount().");
	}

	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, op1->getType());
	llvm::Value* cnt = pop
			? irb.CreateCall(f, {op1})
			: irb.CreateCall(f, {op1, irb.getFalse()});
	if (word)
	{
		cnt = widenFromWord(irb, cnt);
	}
	storeOp(ri->operands[0], cnt, irb);
}

/**
 * Zbb MIN/MINU/MAX/MAXU: signed or unsigned select.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMinMax(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
		case RISCV_INS_MIN:
			cmp = irb.CreateICmpSLT(op1, op2);
			break;
		case RISCV_INS_MINU:
			cmp = irb.CreateICmpULT(op1, op2);
			break;
		case RISCV_INS_MAX:
			cmp = irb.CreateICmpSGT(op1, op2);
			break;
		case RISCV_INS_MAXU:
			cmp = irb.CreateICmpUGT(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateMinMax().");
	}
	storeOp(ri->operands[0], irb.CreateSelect(cmp, op1, op2), irb);
}

/**
 * Zbb ROL/ROR and W/immediate forms. W variants rotate i32 then sign-extend.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateRotate(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool word = i->id == RISCV_INS_ROLW
			|| i->id == RISCV_INS_RORW
			|| i->id == RISCV_INS_RORIW;
	bool left = i->id == RISCV_INS_ROL || i->id == RISCV_INS_ROLW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}
	llvm::Value* res = left ? rotateLeft(irb, op1, op2) : rotateRight(irb, op1, op2);
	if (word)
	{
		res = widenFromWord(irb, res);
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zba SH1ADD/SH2ADD/SH3ADD and *_UW, plus ADD.UW / ZEXT.W / C.ZEXT.W.
 * rd = rs2 + (zext?(rs1) << n). UW/ADD.UW/ZEXT.W zero-extend rs1[31:0] first.
 * Capstone 6 DETAIL_REAL reports C.ZEXT.W as a unary rd (alias ZEXT.W).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateShadd(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	bool zextw = i->id == RISCV_INS_ZEXT_W
			|| i->id == RISCV_INS_ALIAS_ZEXT_W
			|| i->id == RISCV_INS_C_ZEXT_W;
	if (ri->op_count < 1 || (ri->op_count < 2 && !zextw))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned src = ri->op_count >= 2 ? 1u : 0u;
	op1 = loadOp(ri->operands[src], irb);
	op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());
	llvm::Value* rs2 = nullptr;
	if (ri->op_count >= 3)
	{
		rs2 = loadOp(ri->operands[2], irb);
		rs2 = generateTypeConversion(irb, rs2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
	else
	{
		rs2 = llvm::ConstantInt::get(op1->getType(), 0);
	}

	bool uw = i->id == RISCV_INS_SH1ADD_UW
			|| i->id == RISCV_INS_SH2ADD_UW
			|| i->id == RISCV_INS_SH3ADD_UW
			|| i->id == RISCV_INS_ADD_UW
			|| i->id == RISCV_INS_ZEXT_W
			|| i->id == RISCV_INS_ALIAS_ZEXT_W
			|| i->id == RISCV_INS_C_ZEXT_W;
	if (uw)
	{
		op1 = irb.CreateZExt(narrowToWord(irb, op1), getDefaultType());
	}

	unsigned sh = 0;
	switch (i->id)
	{
		case RISCV_INS_SH1ADD:
		case RISCV_INS_SH1ADD_UW:
			sh = 1;
			break;
		case RISCV_INS_SH2ADD:
		case RISCV_INS_SH2ADD_UW:
			sh = 2;
			break;
		case RISCV_INS_SH3ADD:
		case RISCV_INS_SH3ADD_UW:
			sh = 3;
			break;
		case RISCV_INS_ADD_UW:
		case RISCV_INS_ZEXT_W:
		case RISCV_INS_ALIAS_ZEXT_W:
		case RISCV_INS_C_ZEXT_W:
			sh = 0;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateShadd().");
	}
	if (sh != 0)
	{
		op1 = irb.CreateShl(op1, llvm::ConstantInt::get(op1->getType(), sh));
	}
	storeOp(ri->operands[0], irb.CreateAdd(rs2, op1), irb);
}

/**
 * Zbs BCLR/BSET/BINV/BEXT and *I. Index is rs2/imm masked to XLEN-1,
 * matching x86 BTR/BTS/BTC / BT (BEXT writes the bit to rd, not CF).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateZbs(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_BCLR:
		case RISCV_INS_BCLRI:
		{
			auto* bit = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op2);
			res = irb.CreateAnd(op1, irb.CreateNot(bit));
			break;
		}
		case RISCV_INS_BSET:
		case RISCV_INS_BSETI:
		{
			auto* bit = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op2);
			res = irb.CreateOr(op1, bit);
			break;
		}
		case RISCV_INS_BINV:
		case RISCV_INS_BINVI:
		{
			auto* bit = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op2);
			res = irb.CreateXor(op1, bit);
			break;
		}
		case RISCV_INS_BEXT:
		case RISCV_INS_BEXTI:
			res = irb.CreateAnd(
					irb.CreateLShr(op1, op2),
					llvm::ConstantInt::get(op1->getType(), 1));
			break;
		default:
			throw GenericError("Unhandled insn ID in translateZbs().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zba SLLI.UW: rd = zext(rs1[31:0]) << shamt (RV64).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateSlliUw(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op1 = irb.CreateZExt(narrowToWord(irb, op1), getDefaultType());
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateShl(op1, op2), irb);
}

/**
 * Zicond CZERO.EQZ / CZERO.NEZ: rd = (rs2 == 0 / != 0) ? 0 : rs1.
 * Capstone has no CS_MODE_RISCV_ZICOND; the decoder enables it by default.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateCzero(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* zero = llvm::ConstantInt::get(op1->getType(), 0);
	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case RISCV_INS_CZERO_EQZ:
		case RISCV_INS_VT_MASKC:
			cond = irb.CreateICmpEQ(op2, zero);
			break;
		case RISCV_INS_CZERO_NEZ:
		case RISCV_INS_VT_MASKCN:
			cond = irb.CreateICmpNE(op2, zero);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateCzero().");
	}
	storeOp(ri->operands[0], irb.CreateSelect(cond, zero, op1), irb);
}

/**
 * Zbb ORC.B: each byte becomes 0x00 if it was zero, 0xFF otherwise.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateOrcB(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	llvm::Value* res = llvm::ConstantInt::get(ty, 0);
	unsigned nbytes = getArchByteSize();
	for (unsigned b = 0; b < nbytes; ++b)
	{
		auto* byte = irb.CreateAnd(
				irb.CreateLShr(op1, llvm::ConstantInt::get(ty, b * 8)),
				llvm::ConstantInt::get(ty, 0xff));
		auto* nz = irb.CreateICmpNE(byte, llvm::ConstantInt::get(ty, 0));
		auto* fill = irb.CreateSelect(
				nz,
				llvm::ConstantInt::get(ty, 0xffull << (b * 8)),
				llvm::ConstantInt::get(ty, 0));
		res = irb.CreateOr(res, fill);
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbb REV8: byte-swap XLEN.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateRev8(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());
	op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, op1);
	storeOp(ri->operands[0], op1, irb);
}

/**
 * Zbkb BREV8: reverse the bits of each byte (bitreverse then bswap).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBrev8(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	uint64_t m4 = isXlen64() ? 0x0F0F0F0F0F0F0F0Full : 0x0F0F0F0Full;
	uint64_t m2 = isXlen64() ? 0x3333333333333333ull : 0x33333333ull;
	uint64_t m1 = isXlen64() ? 0x5555555555555555ull : 0x55555555ull;
	auto* c4 = llvm::ConstantInt::get(ty, m4);
	auto* c2 = llvm::ConstantInt::get(ty, m2);
	auto* c1 = llvm::ConstantInt::get(ty, m1);
	auto* n4 = llvm::ConstantInt::get(ty, 4);
	auto* n2 = llvm::ConstantInt::get(ty, 2);
	auto* n1 = llvm::ConstantInt::get(ty, 1);
	op1 = irb.CreateOr(
			irb.CreateLShr(irb.CreateAnd(op1, irb.CreateShl(c4, n4)), n4),
			irb.CreateShl(irb.CreateAnd(op1, c4), n4));
	op1 = irb.CreateOr(
			irb.CreateLShr(irb.CreateAnd(op1, irb.CreateShl(c2, n2)), n2),
			irb.CreateShl(irb.CreateAnd(op1, c2), n2));
	op1 = irb.CreateOr(
			irb.CreateLShr(irb.CreateAnd(op1, irb.CreateShl(c1, n1)), n1),
			irb.CreateShl(irb.CreateAnd(op1, c1), n1));
	storeOp(ri->operands[0], op1, irb);
}

/**
 * Zbb SEXT.B/SEXT.H, ZEXT.H and Zcb C.SEXT.B/C.SEXT.H/C.ZEXT.H.
 * ZEXT.W / C.ZEXT.W is ADD.UW rd, rs, x0 (translateShadd).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateSextZext(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned src = ri->op_count >= 2 ? 1u : 0u;
	op1 = loadOp(ri->operands[src], irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_SEXT_B:
		case RISCV_INS_C_SEXT_B:
			res = irb.CreateSExt(irb.CreateTrunc(op1, irb.getInt8Ty()), getDefaultType());
			break;
		case RISCV_INS_SEXT_H:
		case RISCV_INS_C_SEXT_H:
			res = irb.CreateSExt(irb.CreateTrunc(op1, irb.getInt16Ty()), getDefaultType());
			break;
		case RISCV_INS_ZEXT_H:
		case RISCV_INS_C_ZEXT_H:
			res = irb.CreateZExt(irb.CreateTrunc(op1, irb.getInt16Ty()), getDefaultType());
			break;
		case RISCV_INS_ZEXT_W:
		case RISCV_INS_C_ZEXT_W:
			res = irb.CreateZExt(narrowToWord(irb, op1), getDefaultType());
			break;
		default:
			throw GenericError("Unhandled insn ID in translateSextZext().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbkb PACK/PACKH/PACKW (x86 PUNPCKL* / MOVZX parity on GPRs).
 * PACK: rd = {rs2[XLEN/2-1:0], rs1[XLEN/2-1:0]}.
 * PACKH: rd[15:0] = {rs2[7:0], rs1[7:0]}, rest zero (zext).
 * PACKW (RV64): rd = sext_32({rs2[15:0], rs1[15:0]}).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translatePack(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ty = getDefaultType();
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_PACK:
		{
			unsigned halfBits = getArchByteSize() * 4;
			auto* halfTy = irb.getIntNTy(halfBits);
			auto* lo = irb.CreateZExt(irb.CreateTrunc(op1, halfTy), ty);
			auto* hi = irb.CreateShl(
					irb.CreateZExt(irb.CreateTrunc(op2, halfTy), ty),
					llvm::ConstantInt::get(ty, halfBits));
			res = irb.CreateOr(lo, hi);
			break;
		}
		case RISCV_INS_PACKH:
		{
			auto* lo = irb.CreateZExt(irb.CreateTrunc(op1, irb.getInt8Ty()), ty);
			auto* hi = irb.CreateShl(
					irb.CreateZExt(irb.CreateTrunc(op2, irb.getInt8Ty()), ty),
					llvm::ConstantInt::get(ty, 8));
			res = irb.CreateOr(lo, hi);
			break;
		}
		case RISCV_INS_PACKW:
		{
			auto* i32 = irb.getInt32Ty();
			auto* lo = irb.CreateZExt(irb.CreateTrunc(op1, irb.getInt16Ty()), i32);
			auto* hi = irb.CreateShl(
					irb.CreateZExt(irb.CreateTrunc(op2, irb.getInt16Ty()), i32),
					llvm::ConstantInt::get(i32, 16));
			res = widenFromWord(irb, irb.CreateOr(lo, hi));
			break;
		}
		default:
			throw GenericError("Unhandled insn ID in translatePack().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbc CLMUL/CLMULH/CLMULR. Branchless XLEN walk so llvmir-emul can
 * execute the IR (no loops). x86 PCLMULQDQ is already mapped; this is
 * the scalar carry-less product, not the SSE intrinsic.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateClmul(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op2 = irb.CreateZExtOrTrunc(op2, ty);
	unsigned n = ty->getIntegerBitWidth();
	llvm::Value* res = llvm::ConstantInt::get(ty, 0);
	unsigned lo = (i->id == RISCV_INS_CLMULH) ? 1u : 0u;
	unsigned hi = n;
	for (unsigned b = lo; b < hi; ++b)
	{
		auto* bit = irb.CreateAnd(
				irb.CreateLShr(op2, llvm::ConstantInt::get(ty, b)),
				llvm::ConstantInt::get(ty, 1));
		auto* mask = irb.CreateSub(llvm::ConstantInt::get(ty, 0), bit);
		llvm::Value* part = nullptr;
		switch (i->id)
		{
			case RISCV_INS_CLMUL:
				part = irb.CreateShl(op1, llvm::ConstantInt::get(ty, b));
				break;
			case RISCV_INS_CLMULR:
				part = irb.CreateLShr(op1, llvm::ConstantInt::get(ty, b));
				break;
			case RISCV_INS_CLMULH:
				part = irb.CreateLShr(op1, llvm::ConstantInt::get(ty, n - b));
				break;
			default:
				throw GenericError("Unhandled insn ID in translateClmul().");
		}
		res = irb.CreateXor(res, irb.CreateAnd(part, mask));
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * VSETVLI / VSETIVLI / VSETVL: rd = AVL; vl/vtype CSRs if present.
 * Rounding is ignored. Compiler subset: AVL is rs1/uimm (no VLMAX).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateVsetvli(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ri, irb);
	(void)ri->rounding_mode;

	op1 = loadOp(ri->operands[1], irb);
	op2 = loadOp(ri->operands[2], irb);

	if (getRegister(RISCV_REG_VL))
	{
		storeRegister(RISCV_REG_VL, op1, irb);
	}
	else
	{
		auto* ty = op1->getType();
		auto* gv = new llvm::GlobalVariable(
				*_module,
				ty,
				false,
				_regLt,
				llvm::Constant::getNullValue(ty),
				"vl");
		irb.CreateStore(op1, gv);
	}

	if (getRegister(RISCV_REG_VTYPE))
	{
		storeRegister(RISCV_REG_VTYPE, op2, irb);
	}
	else
	{
		auto* ty = op2->getType();
		auto* gv = new llvm::GlobalVariable(
				*_module,
				ty,
				false,
				_regLt,
				llvm::Constant::getNullValue(ty),
				"vtype");
		irb.CreateStore(op2, gv);
	}

	storeOp(ri->operands[0], op1, irb);
}

/**
 * Integer RVV arith: VADD/VSUB/VAND/VOR/VXOR/VMUL .VV/.VX/.VI.
 * VX/VI splat the scalar onto <vscale x 4 x i32>. Mask ignored.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateVarith(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	bool unary = i->id == RISCV_INS_VBREV_V
			|| i->id == RISCV_INS_VBREV8_V
			|| i->id == RISCV_INS_VREV8_V
			|| i->id == RISCV_INS_VCPOP_V
			|| i->id == RISCV_INS_VCLZ_V
			|| i->id == RISCV_INS_VCTZ_V;
	if (ri->op_count < (unary ? 2u : 3u))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = unary
			? llvm::Constant::getNullValue(rvvRegType())
			: splatToRvv(loadOp(ri->operands[2], irb), irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VADD_VV:
		case RISCV_INS_VADD_VX:
		case RISCV_INS_VADD_VI:
			res = irb.CreateAdd(op1, op2);
			break;
		case RISCV_INS_VSUB_VV:
		case RISCV_INS_VSUB_VX:
			res = irb.CreateSub(op1, op2);
			break;
		case RISCV_INS_VAND_VV:
		case RISCV_INS_VAND_VX:
		case RISCV_INS_VAND_VI:
			res = irb.CreateAnd(op1, op2);
			break;
		case RISCV_INS_VANDN_VV:
		case RISCV_INS_VANDN_VX:
			res = irb.CreateAnd(op1, irb.CreateNot(op2));
			break;
		case RISCV_INS_VRSUB_VX:
		case RISCV_INS_VRSUB_VI:
			res = irb.CreateSub(op2, op1);
			break;
		case RISCV_INS_VOR_VV:
		case RISCV_INS_VOR_VX:
		case RISCV_INS_VOR_VI:
			res = irb.CreateOr(op1, op2);
			break;
		case RISCV_INS_VXOR_VV:
		case RISCV_INS_VXOR_VX:
		case RISCV_INS_VXOR_VI:
			res = irb.CreateXor(op1, op2);
			break;
		case RISCV_INS_VMUL_VV:
		case RISCV_INS_VMUL_VX:
			res = irb.CreateMul(op1, op2);
			break;
		case RISCV_INS_VDIV_VV:
		case RISCV_INS_VDIV_VX:
		case RISCV_INS_VDIVU_VV:
		case RISCV_INS_VDIVU_VX:
		case RISCV_INS_VREM_VV:
		case RISCV_INS_VREM_VX:
		case RISCV_INS_VREMU_VV:
		case RISCV_INS_VREMU_VX:
		{
			bool isSigned = i->id == RISCV_INS_VDIV_VV || i->id == RISCV_INS_VDIV_VX
					|| i->id == RISCV_INS_VREM_VV || i->id == RISCV_INS_VREM_VX;
			bool remainder = i->id == RISCV_INS_VREM_VV || i->id == RISCV_INS_VREM_VX
					|| i->id == RISCV_INS_VREMU_VV || i->id == RISCV_INS_VREMU_VX;
			auto* ty = op1->getType();
			auto* zero = llvm::Constant::getNullValue(ty);
			auto* divZero = irb.CreateICmpEQ(op2, zero);
			auto* safe = definedDivisor(op1, op2, isSigned, irb);
			llvm::Value* arith = remainder
					? (isSigned ? irb.CreateSRem(op1, safe) : irb.CreateURem(op1, safe))
					: (isSigned ? irb.CreateSDiv(op1, safe) : irb.CreateUDiv(op1, safe));
			auto* elem = llvm::cast<llvm::VectorType>(ty)->getElementType();
			unsigned bits = llvm::cast<llvm::IntegerType>(elem)->getBitWidth();
			auto* dzRes = remainder
					? op1
					: llvm::Constant::getIntegerValue(ty, llvm::APInt::getAllOnes(bits));
			res = irb.CreateSelect(divZero, dzRes, arith);
			if (isSigned)
			{
				auto* intMin = llvm::Constant::getIntegerValue(ty, llvm::APInt::getSignedMinValue(bits));
				auto* minusOne = llvm::Constant::getIntegerValue(ty, llvm::APInt::getAllOnes(bits));
				auto* overflow = irb.CreateAnd(
						irb.CreateICmpEQ(op1, intMin),
						irb.CreateICmpEQ(op2, minusOne));
				auto* ovRes = remainder ? llvm::Constant::getNullValue(ty) : intMin;
				res = irb.CreateSelect(overflow, ovRes, res);
			}
			break;
		}
		case RISCV_INS_VSLL_VV:
		case RISCV_INS_VSLL_VX:
		case RISCV_INS_VSLL_VI:
			op2 = irb.CreateAnd(op2, splatToRvv(irb.getInt32(31), irb));
			res = irb.CreateShl(op1, op2);
			break;
		case RISCV_INS_VSRL_VV:
		case RISCV_INS_VSRL_VX:
		case RISCV_INS_VSRL_VI:
			op2 = irb.CreateAnd(op2, splatToRvv(irb.getInt32(31), irb));
			res = irb.CreateLShr(op1, op2);
			break;
		case RISCV_INS_VSRA_VV:
		case RISCV_INS_VSRA_VX:
		case RISCV_INS_VSRA_VI:
			op2 = irb.CreateAnd(op2, splatToRvv(irb.getInt32(31), irb));
			res = irb.CreateAShr(op1, op2);
			break;
		case RISCV_INS_VMIN_VV:
		case RISCV_INS_VMIN_VX:
			res = irb.CreateSelect(irb.CreateICmpSLT(op1, op2), op1, op2);
			break;
		case RISCV_INS_VMINU_VV:
		case RISCV_INS_VMINU_VX:
			res = irb.CreateSelect(irb.CreateICmpULT(op1, op2), op1, op2);
			break;
		case RISCV_INS_VMAX_VV:
		case RISCV_INS_VMAX_VX:
			res = irb.CreateSelect(irb.CreateICmpSGT(op1, op2), op1, op2);
			break;
		case RISCV_INS_VMAXU_VV:
		case RISCV_INS_VMAXU_VX:
			res = irb.CreateSelect(irb.CreateICmpUGT(op1, op2), op1, op2);
			break;
		case RISCV_INS_VMULH_VV:
		case RISCV_INS_VMULH_VX:
		case RISCV_INS_VMULHU_VV:
		case RISCV_INS_VMULHU_VX:
		case RISCV_INS_VMULHSU_VV:
		case RISCV_INS_VMULHSU_VX:
		{
			auto* wty = llvm::ScalableVectorType::get(irb.getInt64Ty(), 4);
			bool sa = i->id != RISCV_INS_VMULHU_VV && i->id != RISCV_INS_VMULHU_VX;
			bool sb = i->id == RISCV_INS_VMULH_VV || i->id == RISCV_INS_VMULH_VX;
			auto* a = sa ? irb.CreateSExt(op1, wty) : irb.CreateZExt(op1, wty);
			auto* b = sb ? irb.CreateSExt(op2, wty) : irb.CreateZExt(op2, wty);
			auto* sh = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), 32));
			res = irb.CreateTrunc(irb.CreateLShr(irb.CreateMul(a, b), sh), rvvRegType());
			break;
		}
		case RISCV_INS_VAADD_VV:
		case RISCV_INS_VAADD_VX:
		case RISCV_INS_VAADDU_VV:
		case RISCV_INS_VAADDU_VX:
		case RISCV_INS_VASUB_VV:
		case RISCV_INS_VASUB_VX:
		case RISCV_INS_VASUBU_VV:
		case RISCV_INS_VASUBU_VX:
		{
			auto* wty = llvm::ScalableVectorType::get(irb.getInt64Ty(), 4);
			bool isSigned = i->id == RISCV_INS_VAADD_VV || i->id == RISCV_INS_VAADD_VX
					|| i->id == RISCV_INS_VASUB_VV || i->id == RISCV_INS_VASUB_VX;
			bool isSub = i->id == RISCV_INS_VASUB_VV || i->id == RISCV_INS_VASUB_VX
					|| i->id == RISCV_INS_VASUBU_VV || i->id == RISCV_INS_VASUBU_VX;
			auto* a = isSigned ? irb.CreateSExt(op1, wty) : irb.CreateZExt(op1, wty);
			auto* b = isSigned ? irb.CreateSExt(op2, wty) : irb.CreateZExt(op2, wty);
			auto* one = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), 1));
			auto* sum = isSub ? irb.CreateSub(a, b) : irb.CreateAdd(a, b);
			sum = irb.CreateAdd(sum, one);
			auto* avg = isSigned ? irb.CreateAShr(sum, one) : irb.CreateLShr(sum, one);
			res = irb.CreateTrunc(avg, rvvRegType());
			break;
		}
		case RISCV_INS_VSADD_VV:
		case RISCV_INS_VSADD_VX:
		case RISCV_INS_VSADD_VI:
		case RISCV_INS_VSADDU_VV:
		case RISCV_INS_VSADDU_VX:
		case RISCV_INS_VSADDU_VI:
		case RISCV_INS_VSSUB_VV:
		case RISCV_INS_VSSUB_VX:
		case RISCV_INS_VSSUBU_VV:
		case RISCV_INS_VSSUBU_VX:
		{
			llvm::Intrinsic::ID iid = llvm::Intrinsic::sadd_sat;
			if (i->id == RISCV_INS_VSADDU_VV || i->id == RISCV_INS_VSADDU_VX
					|| i->id == RISCV_INS_VSADDU_VI)
			{
				iid = llvm::Intrinsic::uadd_sat;
			}
			else if (i->id == RISCV_INS_VSSUB_VV || i->id == RISCV_INS_VSSUB_VX)
			{
				iid = llvm::Intrinsic::ssub_sat;
			}
			else if (i->id == RISCV_INS_VSSUBU_VV || i->id == RISCV_INS_VSSUBU_VX)
			{
				iid = llvm::Intrinsic::usub_sat;
			}
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					iid,
					{rvvRegType()});
			res = irb.CreateCall(fn, {op1, op2});
			break;
		}
		case RISCV_INS_VMACC_VV:
		case RISCV_INS_VMACC_VX:
		case RISCV_INS_VNMSAC_VV:
		case RISCV_INS_VNMSAC_VX:
		case RISCV_INS_VMADD_VV:
		case RISCV_INS_VMADD_VX:
		case RISCV_INS_VNMSUB_VV:
		case RISCV_INS_VNMSUB_VX:
		{
			auto* acc = splatToRvv(loadOp(ri->operands[0], irb), irb);
			if (i->id == RISCV_INS_VMADD_VV || i->id == RISCV_INS_VMADD_VX)
			{
				res = irb.CreateAdd(irb.CreateMul(op1, acc), op2);
			}
			else if (i->id == RISCV_INS_VNMSUB_VV || i->id == RISCV_INS_VNMSUB_VX)
			{
				res = irb.CreateSub(op2, irb.CreateMul(op1, acc));
			}
			else if (i->id == RISCV_INS_VNMSAC_VV || i->id == RISCV_INS_VNMSAC_VX)
			{
				res = irb.CreateSub(acc, irb.CreateMul(op1, op2));
			}
			else
			{
				res = irb.CreateAdd(acc, irb.CreateMul(op1, op2));
			}
			break;
		}
		case RISCV_INS_VSMUL_VV:
		case RISCV_INS_VSMUL_VX:
		{
			auto* wty = llvm::ScalableVectorType::get(irb.getInt64Ty(), 4);
			auto* a = irb.CreateSExt(op1, wty);
			auto* b = irb.CreateSExt(op2, wty);
			auto* rnd = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), 1ull << 30));
			auto* sh = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), 31));
			auto* hi = irb.CreateAShr(irb.CreateAdd(irb.CreateMul(a, b), rnd), sh);
			auto* min = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), llvm::APInt(64, 0xffffffff80000000ull, true)));
			auto* max = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), 0x7fffffff));
			hi = irb.CreateSelect(irb.CreateICmpSLT(hi, min), min, hi);
			hi = irb.CreateSelect(irb.CreateICmpSGT(hi, max), max, hi);
			res = irb.CreateTrunc(hi, rvvRegType());
			break;
		}
		case RISCV_INS_VSSRL_VV:
		case RISCV_INS_VSSRL_VX:
		case RISCV_INS_VSSRL_VI:
		case RISCV_INS_VSSRA_VV:
		case RISCV_INS_VSSRA_VX:
		case RISCV_INS_VSSRA_VI:
		{
			auto* sh = irb.CreateAnd(op2, splatToRvv(irb.getInt32(31), irb));
			auto* zero = llvm::Constant::getNullValue(rvvRegType());
			auto* one = splatToRvv(irb.getInt32(1), irb);
			auto* addend = irb.CreateSelect(
					irb.CreateICmpEQ(sh, zero),
					zero,
					irb.CreateShl(one, irb.CreateSub(sh, one)));
			auto* sum = irb.CreateAdd(op1, addend);
			bool arith = i->id == RISCV_INS_VSSRA_VV
					|| i->id == RISCV_INS_VSSRA_VX
					|| i->id == RISCV_INS_VSSRA_VI;
			res = arith ? irb.CreateAShr(sum, sh) : irb.CreateLShr(sum, sh);
			break;
		}
		case RISCV_INS_VROR_VV:
		case RISCV_INS_VROR_VX:
		case RISCV_INS_VROR_VI:
		case RISCV_INS_VROL_VV:
		case RISCV_INS_VROL_VX:
		{
			auto* sh = irb.CreateAnd(op2, splatToRvv(irb.getInt32(31), irb));
			auto* rsh = irb.CreateAnd(
					irb.CreateSub(splatToRvv(irb.getInt32(32), irb), sh),
					splatToRvv(irb.getInt32(31), irb));
			bool left = i->id == RISCV_INS_VROL_VV || i->id == RISCV_INS_VROL_VX;
			auto* a = left ? irb.CreateShl(op1, sh) : irb.CreateLShr(op1, sh);
			auto* b = left ? irb.CreateLShr(op1, rsh) : irb.CreateShl(op1, rsh);
			res = irb.CreateOr(a, b);
			break;
		}
		case RISCV_INS_VCLMUL_VV:
		case RISCV_INS_VCLMUL_VX:
		case RISCV_INS_VCLMULH_VV:
		case RISCV_INS_VCLMULH_VX:
		{
			auto* ty = rvvRegType();
			res = llvm::Constant::getNullValue(ty);
			unsigned lo = (i->id == RISCV_INS_VCLMULH_VV || i->id == RISCV_INS_VCLMULH_VX) ? 1u : 0u;
			for (unsigned b = lo; b < 32; ++b)
			{
				auto* bit = irb.CreateAnd(
						irb.CreateLShr(op2, splatToRvv(irb.getInt32(b), irb)),
						splatToRvv(irb.getInt32(1), irb));
				auto* mask = irb.CreateSub(llvm::Constant::getNullValue(ty), bit);
				llvm::Value* part = nullptr;
				if (i->id == RISCV_INS_VCLMULH_VV || i->id == RISCV_INS_VCLMULH_VX)
				{
					part = irb.CreateLShr(op1, splatToRvv(irb.getInt32(32 - b), irb));
				}
				else
				{
					part = irb.CreateShl(op1, splatToRvv(irb.getInt32(b), irb));
				}
				res = irb.CreateXor(res, irb.CreateAnd(part, mask));
			}
			break;
		}
		case RISCV_INS_VBREV_V:
		{
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					llvm::Intrinsic::bitreverse,
					{rvvRegType()});
			res = irb.CreateCall(fn, {op1});
			break;
		}
		case RISCV_INS_VBREV8_V:
		{
			auto* i8ty = llvm::ScalableVectorType::get(irb.getInt8Ty(), 16);
			auto* bytes = irb.CreateBitCast(op1, i8ty);
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					llvm::Intrinsic::bitreverse,
					{i8ty});
			res = irb.CreateBitCast(irb.CreateCall(fn, {bytes}), rvvRegType());
			break;
		}
		case RISCV_INS_VREV8_V:
		{
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					llvm::Intrinsic::bswap,
					{rvvRegType()});
			res = irb.CreateCall(fn, {op1});
			break;
		}
		case RISCV_INS_VCPOP_V:
		{
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					llvm::Intrinsic::ctpop,
					{rvvRegType()});
			res = irb.CreateCall(fn, {op1});
			break;
		}
		case RISCV_INS_VCLZ_V:
		case RISCV_INS_VCTZ_V:
		{
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					(i->id == RISCV_INS_VCLZ_V)
							? llvm::Intrinsic::ctlz
							: llvm::Intrinsic::cttz,
					{rvvRegType()});
			res = irb.CreateCall(fn, {op1, irb.getFalse()});
			break;
		}
		default:
			throw GenericError("Unhandled insn ID in translateVarith().");
	}
	storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
}

/**
 * VLE8/16/32/64.V: load then bitcast into the <vscale x 4 x i32> V-reg.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateVload(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned bits = 32;
	switch (i->id)
	{
		case RISCV_INS_VLE8_V:
		case RISCV_INS_VLE8FF_V:
		case RISCV_INS_VLM_V:
			bits = 8;
			break;
		case RISCV_INS_VLE16_V:
		case RISCV_INS_VLE16FF_V:
			bits = 16;
			break;
		case RISCV_INS_VLE64_V:
		case RISCV_INS_VLE64FF_V:
			bits = 64;
			break;
		case RISCV_INS_VLE32FF_V:
			bits = 32;
			break;
		default: break;
	}
	auto* memTy = rvvMemVecType(bits);
	auto* addr = rvvMemAddr(ri->operands[1], irb);
	op1 = loadIntPtr(irb, addr, memTy);
	if (op1->getType() != rvvRegType())
	{
		op1 = irb.CreateBitCast(op1, rvvRegType());
	}
	storeOp(ri->operands[0], op1, irb, eOpConv::NOTHING);
}

/**
 * VSE8/16/32/64.V: bitcast the V-reg to the memory vector type and store.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateVstore(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned bits = 32;
	switch (i->id)
	{
		case RISCV_INS_VSE8_V:
		case RISCV_INS_VSM_V:
			bits = 8;
			break;
		case RISCV_INS_VSE16_V: bits = 16; break;
		case RISCV_INS_VSE64_V: bits = 64; break;
		default: break;
	}
	op0 = loadOp(ri->operands[0], irb);
	auto* memTy = rvvMemVecType(bits);
	if (op0->getType() != memTy)
	{
		op0 = irb.CreateBitCast(op0, memTy);
	}
	auto* addr = rvvMemAddr(ri->operands[1], irb);
	storeIntPtr(irb, op0, addr, memTy);
}

/**
 * VMV.V.V / VMV.V.X: copy or splat into vd. Two Capstone operands.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateVmv(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* src = splatRvvFromScalar(loadOp(ri->operands[1], irb), irb, irb.getInt32Ty());
	op1 = splatToRvv(src, irb);
	storeOp(ri->operands[0], op1, irb, eOpConv::NOTHING);
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::rvvFpType()
{
	return llvm::ScalableVectorType::get(
			llvm::Type::getFloatTy(_module->getContext()),
			4);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::asRvvFp(
		llvm::Value* v,
		llvm::IRBuilder<>& irb)
{
	auto* fty = rvvFpType();
	if (v->getType() == fty)
	{
		return v;
	}
	if (v->getType()->isVectorTy())
	{
		return irb.CreateBitCast(v, fty);
	}
	return splatToRvvFp(v, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::fromRvvFp(
		llvm::Value* v,
		llvm::IRBuilder<>& irb)
{
	if (v->getType() == rvvRegType())
	{
		return v;
	}
	return irb.CreateBitCast(v, rvvRegType());
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::splatToRvvFp(
		llvm::Value* s,
		llvm::IRBuilder<>& irb)
{
	auto* fty = llvm::cast<llvm::VectorType>(rvvFpType());
	auto* elem = fty->getElementType();
	if (s->getType()->isVectorTy())
	{
		return asRvvFp(s, irb);
	}
	if (s->getType()->isFloatingPointTy())
	{
		s = irb.CreateFPCast(s, elem);
	}
	else
	{
		s = irb.CreateBitCast(irb.CreateTrunc(s, irb.getInt32Ty()), elem);
	}
	return irb.CreateVectorSplat(fty->getElementCount(), s);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVfarith(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	bool unary = i->id == RISCV_INS_VFSQRT_V
			|| i->id == RISCV_INS_VFCLASS_V
			|| i->id == RISCV_INS_VFREC7_V
			|| i->id == RISCV_INS_VFRSQRT7_V;
	if (ri->op_count < (unary ? 2u : 3u))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* a = asRvvFp(loadOp(ri->operands[1], irb), irb);
	llvm::Value* b = nullptr;
	if (!unary)
	{
		b = asRvvFp(loadOp(ri->operands[2], irb), irb);
	}
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VFADD_VV:
		case RISCV_INS_VFADD_VF:
			res = irb.CreateFAdd(a, b);
			break;
		case RISCV_INS_VFSUB_VV:
		case RISCV_INS_VFSUB_VF:
			res = irb.CreateFSub(a, b);
			break;
		case RISCV_INS_VFRSUB_VF:
			res = irb.CreateFSub(b, a);
			break;
		case RISCV_INS_VFMUL_VV:
		case RISCV_INS_VFMUL_VF:
			res = irb.CreateFMul(a, b);
			break;
		case RISCV_INS_VFDIV_VV:
		case RISCV_INS_VFDIV_VF:
		case RISCV_INS_VFRDIV_VF:
		{
			auto* num = (i->id == RISCV_INS_VFRDIV_VF) ? b : a;
			auto* den = (i->id == RISCV_INS_VFRDIV_VF) ? a : b;
			auto* zero = llvm::Constant::getNullValue(den->getType());
			auto* one = llvm::ConstantFP::get(den->getType(), 1.0);
			auto* safe = irb.CreateSelect(irb.CreateFCmpOEQ(den, zero), one, den);
			res = irb.CreateFDiv(num, safe);
			break;
		}
		case RISCV_INS_VFMIN_VV:
		case RISCV_INS_VFMIN_VF:
			res = irb.CreateSelect(irb.CreateFCmpOLT(a, b), a, b);
			break;
		case RISCV_INS_VFMAX_VV:
		case RISCV_INS_VFMAX_VF:
			res = irb.CreateSelect(irb.CreateFCmpOGT(a, b), a, b);
			break;
		case RISCV_INS_VFSQRT_V:
			res = irb.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, a);
			break;
		case RISCV_INS_VFREC7_V:
		case RISCV_INS_VFRSQRT7_V:
		{
			llvm::Value* den = a;
			if (i->id == RISCV_INS_VFRSQRT7_V)
			{
				den = irb.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, a);
			}
			auto* zero = llvm::Constant::getNullValue(den->getType());
			auto* one = llvm::ConstantFP::get(den->getType(), 1.0);
			auto* safe = irb.CreateSelect(irb.CreateFCmpOEQ(den, zero), one, den);
			res = irb.CreateFDiv(one, safe);
			break;
		}
		case RISCV_INS_VFSGNJ_VV:
		case RISCV_INS_VFSGNJ_VF:
		case RISCV_INS_VFSGNJN_VV:
		case RISCV_INS_VFSGNJN_VF:
		case RISCV_INS_VFSGNJX_VV:
		case RISCV_INS_VFSGNJX_VF:
		{
			auto* ai = irb.CreateBitCast(a, rvvRegType());
			auto* bi = irb.CreateBitCast(b, rvvRegType());
			auto* signBit = splatToRvv(irb.getInt32(0x80000000u), irb);
			auto* mag = irb.CreateAnd(ai, splatToRvv(irb.getInt32(0x7fffffffu), irb));
			llvm::Value* sign = irb.CreateAnd(bi, signBit);
			if (i->id == RISCV_INS_VFSGNJN_VV || i->id == RISCV_INS_VFSGNJN_VF)
			{
				sign = irb.CreateXor(sign, signBit);
			}
			else if (i->id == RISCV_INS_VFSGNJX_VV || i->id == RISCV_INS_VFSGNJX_VF)
			{
				sign = irb.CreateAnd(irb.CreateXor(ai, bi), signBit);
			}
			res = fromRvvFp(irb.CreateOr(mag, sign), irb);
			storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
			return;
		}
		case RISCV_INS_VFCLASS_V:
		{
			auto* bits = irb.CreateBitCast(a, rvvRegType());
			auto* z = llvm::Constant::getNullValue(rvvRegType());
			auto* signMask = splatToRvv(irb.getInt32(0x80000000u), irb);
			auto* expMask = splatToRvv(irb.getInt32(0x7f800000u), irb);
			auto* fracMask = splatToRvv(irb.getInt32(0x007fffffu), irb);
			auto* qnanBit = splatToRvv(irb.getInt32(0x00400000u), irb);
			auto* sign = irb.CreateAnd(bits, signMask);
			auto* exp = irb.CreateAnd(bits, expMask);
			auto* frac = irb.CreateAnd(bits, fracMask);
			auto* qbit = irb.CreateAnd(bits, qnanBit);
			auto* isNeg = irb.CreateICmpNE(sign, z);
			auto* isPos = irb.CreateICmpEQ(sign, z);
			auto* exp0 = irb.CreateICmpEQ(exp, z);
			auto* expMax = irb.CreateICmpEQ(exp, expMask);
			auto* frac0 = irb.CreateICmpEQ(frac, z);
			auto* fracNz = irb.CreateICmpNE(frac, z);
			auto* expNorm = irb.CreateAnd(
					irb.CreateICmpNE(exp, z),
					irb.CreateICmpNE(exp, expMask));
			auto* isQuiet = irb.CreateICmpNE(qbit, z);
			auto* isSig = irb.CreateICmpEQ(qbit, z);
			auto classBit = [&](llvm::Value* cond, unsigned b)
			{
				return irb.CreateSelect(
						cond,
						splatToRvv(irb.getInt32(1u << b), irb),
						z);
			};
			res = classBit(irb.CreateAnd(isNeg, irb.CreateAnd(expMax, frac0)), 0);
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, expNorm), 1));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, irb.CreateAnd(exp0, fracNz)), 2));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, irb.CreateAnd(exp0, frac0)), 3));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(exp0, frac0)), 4));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(exp0, fracNz)), 5));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, expNorm), 6));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(expMax, frac0)), 7));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(expMax, irb.CreateAnd(fracNz, isSig)), 8));
			res = irb.CreateOr(res, classBit(irb.CreateAnd(expMax, irb.CreateAnd(fracNz, isQuiet)), 9));
			storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
			return;
		}
		default:
			throw GenericError("Unhandled insn ID in translateVfarith().");
	}
	storeOp(ri->operands[0], fromRvvFp(res, irb), irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVfma(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	unsigned accIdx = 0;
	unsigned aIdx = 1;
	unsigned bIdx = 2;
	if (ri->op_count >= 4)
	{
		accIdx = 1;
		aIdx = 2;
		bIdx = 3;
	}
	else if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* acc = asRvvFp(loadOp(ri->operands[accIdx], irb), irb);
	auto* a = asRvvFp(loadOp(ri->operands[aIdx], irb), irb);
	auto* b = asRvvFp(loadOp(ri->operands[bIdx], irb), irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VFMACC_VV:
		case RISCV_INS_VFMACC_VF:
			res = irb.CreateFAdd(irb.CreateFMul(a, b), acc);
			break;
		case RISCV_INS_VFMADD_VV:
		case RISCV_INS_VFMADD_VF:
			res = irb.CreateFAdd(irb.CreateFMul(a, acc), b);
			break;
		case RISCV_INS_VFMSAC_VV:
		case RISCV_INS_VFMSAC_VF:
			res = irb.CreateFSub(irb.CreateFMul(a, b), acc);
			break;
		case RISCV_INS_VFMSUB_VV:
		case RISCV_INS_VFMSUB_VF:
			res = irb.CreateFSub(irb.CreateFMul(a, acc), b);
			break;
		case RISCV_INS_VFNMACC_VV:
		case RISCV_INS_VFNMACC_VF:
			res = irb.CreateFSub(
					llvm::Constant::getNullValue(a->getType()),
					irb.CreateFAdd(irb.CreateFMul(a, b), acc));
			break;
		case RISCV_INS_VFNMADD_VV:
		case RISCV_INS_VFNMADD_VF:
			res = irb.CreateFSub(
					llvm::Constant::getNullValue(a->getType()),
					irb.CreateFAdd(irb.CreateFMul(a, acc), b));
			break;
		case RISCV_INS_VFNMSAC_VV:
		case RISCV_INS_VFNMSAC_VF:
			res = irb.CreateFSub(acc, irb.CreateFMul(a, b));
			break;
		case RISCV_INS_VFNMSUB_VV:
		case RISCV_INS_VFNMSUB_VF:
			res = irb.CreateFSub(b, irb.CreateFMul(a, acc));
			break;
		case RISCV_INS_VFWMACCBF16_VV:
		case RISCV_INS_VFWMACCBF16_VF:
		{
			acc = asRvvFp(loadOp(ri->operands[accIdx], irb), irb);
			auto* vs2 = splatToRvv(loadOp(ri->operands[aIdx], irb), irb);
			a = bf16LanesToF32(vs2, irb);
			auto* src1 = loadOp(ri->operands[bIdx], irb);
			if (i->id == RISCV_INS_VFWMACCBF16_VF)
			{
				b = bf16ScalarToF32Vec(src1, irb);
			}
			else
			{
				b = bf16LanesToF32(splatToRvv(src1, irb), irb);
			}
			auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
					_module,
					llvm::Intrinsic::fma,
					{rvvFpType()});
			res = irb.CreateCall(fn, {a, b, acc});
			break;
		}
		default:
			throw GenericError("Unhandled insn ID in translateVfma().");
	}
	storeOp(ri->operands[0], fromRvvFp(res, irb), irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVcmp(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(
			splatRvvFromScalar(loadOp(ri->operands[1], irb), irb, irb.getInt32Ty()),
			irb);
	op2 = splatToRvv(
			splatRvvFromScalar(loadOp(ri->operands[2], irb), irb, irb.getInt32Ty()),
			irb);
	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VMSEQ_VV:
		case RISCV_INS_VMSEQ_VX:
		case RISCV_INS_VMSEQ_VI:
			cmp = irb.CreateICmpEQ(op1, op2);
			break;
		case RISCV_INS_VMSNE_VV:
		case RISCV_INS_VMSNE_VX:
		case RISCV_INS_VMSNE_VI:
			cmp = irb.CreateICmpNE(op1, op2);
			break;
		case RISCV_INS_VMSLT_VV:
		case RISCV_INS_VMSLT_VX:
		case RISCV_INS_VMSLT_VI:
			cmp = irb.CreateICmpSLT(op1, op2);
			break;
		case RISCV_INS_VMSLE_VV:
		case RISCV_INS_VMSLE_VX:
		case RISCV_INS_VMSLE_VI:
			cmp = irb.CreateICmpSLE(op1, op2);
			break;
		case RISCV_INS_VMSGT_VX:
		case RISCV_INS_VMSGT_VI:
			cmp = irb.CreateICmpSGT(op1, op2);
			break;
		case RISCV_INS_VMSLTU_VV:
		case RISCV_INS_VMSLTU_VX:
			cmp = irb.CreateICmpULT(op1, op2);
			break;
		case RISCV_INS_VMSLEU_VV:
		case RISCV_INS_VMSLEU_VX:
		case RISCV_INS_VMSLEU_VI:
			cmp = irb.CreateICmpULE(op1, op2);
			break;
		case RISCV_INS_VMSGTU_VX:
		case RISCV_INS_VMSGTU_VI:
			cmp = irb.CreateICmpUGT(op1, op2);
			break;
		case RISCV_INS_VMFEQ_VV:
		case RISCV_INS_VMFEQ_VF:
		case RISCV_INS_VMFNE_VV:
		case RISCV_INS_VMFNE_VF:
		case RISCV_INS_VMFLT_VV:
		case RISCV_INS_VMFLT_VF:
		case RISCV_INS_VMFLE_VV:
		case RISCV_INS_VMFLE_VF:
		case RISCV_INS_VMFGT_VF:
		case RISCV_INS_VMFGE_VF:
		{
			auto* a = asRvvFp(op1, irb);
			auto* b = asRvvFp(op2, irb);
			if (i->id == RISCV_INS_VMFEQ_VV || i->id == RISCV_INS_VMFEQ_VF)
			{
				cmp = irb.CreateFCmpOEQ(a, b);
			}
			else if (i->id == RISCV_INS_VMFNE_VV || i->id == RISCV_INS_VMFNE_VF)
			{
				cmp = irb.CreateFCmpUNE(a, b);
			}
			else if (i->id == RISCV_INS_VMFLT_VV || i->id == RISCV_INS_VMFLT_VF)
			{
				cmp = irb.CreateFCmpOLT(a, b);
			}
			else if (i->id == RISCV_INS_VMFLE_VV || i->id == RISCV_INS_VMFLE_VF)
			{
				cmp = irb.CreateFCmpOLE(a, b);
			}
			else if (i->id == RISCV_INS_VMFGT_VF)
			{
				cmp = irb.CreateFCmpOGT(a, b);
			}
			else
			{
				cmp = irb.CreateFCmpOGE(a, b);
			}
			break;
		}
		default:
			throw GenericError("Unhandled insn ID in translateVcmp().");
	}
	storeOp(
			ri->operands[0],
			irb.CreateZExt(cmp, rvvRegType()),
			irb,
			eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVslide(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	auto* off = loadOp(ri->operands[2], irb);
	off = irb.CreateZExtOrTrunc(off, irb.getInt32Ty());
	auto* four = irb.getInt32(4);
	off = irb.CreateSelect(irb.CreateICmpULT(off, four), off, four);
	auto* bits = irb.CreateMul(off, irb.getInt32(32));
	auto* fatTy = llvm::ScalableVectorType::get(irb.getIntNTy(128), 1);
	auto* fat = irb.CreateBitCast(op1, fatTy);
	auto* amt = irb.CreateVectorSplat(
			llvm::cast<llvm::VectorType>(fatTy)->getElementCount(),
			irb.CreateZExt(bits, irb.getIntNTy(128)));
	bool up = i->id == RISCV_INS_VSLIDEUP_VX || i->id == RISCV_INS_VSLIDEUP_VI;
	auto* shifted = up ? irb.CreateShl(fat, amt) : irb.CreateLShr(fat, amt);
	storeOp(ri->operands[0], irb.CreateBitCast(shifted, rvvRegType()), irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVwiden(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = splatToRvv(loadOp(ri->operands[2], irb), irb);
	auto* w2 = llvm::ScalableVectorType::get(irb.getInt64Ty(), 2);
	bool isWsll = i->id == RISCV_INS_VWSLL_VV
			|| i->id == RISCV_INS_VWSLL_VX
			|| i->id == RISCV_INS_VWSLL_VI;
	bool vs2Wide = i->id == RISCV_INS_VWADD_WV || i->id == RISCV_INS_VWADD_WX
			|| i->id == RISCV_INS_VWADDU_WV || i->id == RISCV_INS_VWADDU_WX
			|| i->id == RISCV_INS_VWSUB_WV || i->id == RISCV_INS_VWSUB_WX
			|| i->id == RISCV_INS_VWSUBU_WV || i->id == RISCV_INS_VWSUBU_WX;
	bool vs2u = i->id == RISCV_INS_VWADDU_VV || i->id == RISCV_INS_VWADDU_VX
			|| i->id == RISCV_INS_VWADDU_WV || i->id == RISCV_INS_VWADDU_WX
			|| i->id == RISCV_INS_VWSUBU_VV || i->id == RISCV_INS_VWSUBU_VX
			|| i->id == RISCV_INS_VWSUBU_WV || i->id == RISCV_INS_VWSUBU_WX
			|| i->id == RISCV_INS_VWMULU_VV || i->id == RISCV_INS_VWMULU_VX
			|| i->id == RISCV_INS_VWMACCU_VV || i->id == RISCV_INS_VWMACCU_VX
			|| i->id == RISCV_INS_VWMACCUS_VX
			|| i->id == RISCV_INS_VWSLL_VV || i->id == RISCV_INS_VWSLL_VX
			|| i->id == RISCV_INS_VWSLL_VI;
	bool vs1u = i->id == RISCV_INS_VWADDU_VV || i->id == RISCV_INS_VWADDU_VX
			|| i->id == RISCV_INS_VWADDU_WV || i->id == RISCV_INS_VWADDU_WX
			|| i->id == RISCV_INS_VWSUBU_VV || i->id == RISCV_INS_VWSUBU_VX
			|| i->id == RISCV_INS_VWSUBU_WV || i->id == RISCV_INS_VWSUBU_WX
			|| i->id == RISCV_INS_VWMULU_VV || i->id == RISCV_INS_VWMULU_VX
			|| i->id == RISCV_INS_VWMULSU_VV || i->id == RISCV_INS_VWMULSU_VX
			|| i->id == RISCV_INS_VWMACCU_VV || i->id == RISCV_INS_VWMACCU_VX
			|| i->id == RISCV_INS_VWMACCSU_VV || i->id == RISCV_INS_VWMACCSU_VX;
	bool isSub = i->id == RISCV_INS_VWSUB_VV || i->id == RISCV_INS_VWSUB_VX
			|| i->id == RISCV_INS_VWSUB_WV || i->id == RISCV_INS_VWSUB_WX
			|| i->id == RISCV_INS_VWSUBU_VV || i->id == RISCV_INS_VWSUBU_VX
			|| i->id == RISCV_INS_VWSUBU_WV || i->id == RISCV_INS_VWSUBU_WX;
	bool isMul = i->id == RISCV_INS_VWMUL_VV || i->id == RISCV_INS_VWMUL_VX
			|| i->id == RISCV_INS_VWMULU_VV || i->id == RISCV_INS_VWMULU_VX
			|| i->id == RISCV_INS_VWMULSU_VV || i->id == RISCV_INS_VWMULSU_VX;
	bool isMacc = i->id == RISCV_INS_VWMACC_VV || i->id == RISCV_INS_VWMACC_VX
			|| i->id == RISCV_INS_VWMACCU_VV || i->id == RISCV_INS_VWMACCU_VX
			|| i->id == RISCV_INS_VWMACCSU_VV || i->id == RISCV_INS_VWMACCSU_VX
			|| i->id == RISCV_INS_VWMACCUS_VX;

	auto widen = [&](llvm::Value* e, bool isUnsigned)
	{
		return isUnsigned
				? irb.CreateZExt(e, irb.getInt64Ty())
				: irb.CreateSExt(e, irb.getInt64Ty());
	};

	llvm::Value* a0 = nullptr;
	llvm::Value* a1 = nullptr;
	if (vs2Wide)
	{
		auto* w = irb.CreateBitCast(op1, w2);
		a0 = irb.CreateExtractElement(w, irb.getInt32(0));
		a1 = irb.CreateExtractElement(w, irb.getInt32(1));
	}
	else
	{
		a0 = widen(irb.CreateExtractElement(op1, irb.getInt32(0)), vs2u);
		a1 = widen(irb.CreateExtractElement(op1, irb.getInt32(1)), vs2u);
	}
	auto* b0 = widen(irb.CreateExtractElement(op2, irb.getInt32(0)), vs1u);
	auto* b1 = widen(irb.CreateExtractElement(op2, irb.getInt32(1)), vs1u);

	llvm::Value* s0 = nullptr;
	llvm::Value* s1 = nullptr;
	if (isWsll)
	{
		auto* sh0 = irb.CreateAnd(irb.CreateZExt(irb.CreateExtractElement(op2, irb.getInt32(0)), irb.getInt64Ty()), llvm::ConstantInt::get(irb.getInt64Ty(), 63));
		auto* sh1 = irb.CreateAnd(irb.CreateZExt(irb.CreateExtractElement(op2, irb.getInt32(1)), irb.getInt64Ty()), llvm::ConstantInt::get(irb.getInt64Ty(), 63));
		s0 = irb.CreateShl(a0, sh0);
		s1 = irb.CreateShl(a1, sh1);
	}
	else if (isMul || isMacc)
	{
		s0 = irb.CreateMul(a0, b0);
		s1 = irb.CreateMul(a1, b1);
		if (isMacc)
		{
			auto* acc = irb.CreateBitCast(
					splatToRvv(loadOp(ri->operands[0], irb), irb),
					w2);
			s0 = irb.CreateAdd(irb.CreateExtractElement(acc, irb.getInt32(0)), s0);
			s1 = irb.CreateAdd(irb.CreateExtractElement(acc, irb.getInt32(1)), s1);
		}
	}
	else if (isSub)
	{
		s0 = irb.CreateSub(a0, b0);
		s1 = irb.CreateSub(a1, b1);
	}
	else
	{
		s0 = irb.CreateAdd(a0, b0);
		s1 = irb.CreateAdd(a1, b1);
	}
	llvm::Value* p = llvm::PoisonValue::get(w2);
	p = irb.CreateInsertElement(p, s0, irb.getInt32(0));
	p = irb.CreateInsertElement(p, s1, irb.getInt32(1));
	storeOp(ri->operands[0], irb.CreateBitCast(p, rvvRegType()), irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVnarrow(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* w2 = llvm::ScalableVectorType::get(irb.getInt64Ty(), 2);
	op1 = irb.CreateBitCast(splatToRvv(loadOp(ri->operands[1], irb), irb), w2);
	auto* sh = loadOp(ri->operands[2], irb);
	if (!sh->getType()->isVectorTy())
	{
		sh = irb.CreateZExtOrTrunc(sh, irb.getInt64Ty());
		sh = irb.CreateAnd(sh, llvm::ConstantInt::get(sh->getType(), 63));
		sh = irb.CreateVectorSplat(llvm::cast<llvm::VectorType>(w2)->getElementCount(), sh);
	}
	else
	{
		sh = irb.CreateZExtOrTrunc(irb.CreateExtractElement(splatToRvv(sh, irb), irb.getInt32(0)), irb.getInt64Ty());
		sh = irb.CreateAnd(sh, llvm::ConstantInt::get(sh->getType(), 63));
		sh = irb.CreateVectorSplat(llvm::cast<llvm::VectorType>(w2)->getElementCount(), sh);
	}
	bool arith = i->id == RISCV_INS_VNSRA_WV
			|| i->id == RISCV_INS_VNSRA_WX
			|| i->id == RISCV_INS_VNSRA_WI
			|| i->id == RISCV_INS_VNCLIP_WV
			|| i->id == RISCV_INS_VNCLIP_WX
			|| i->id == RISCV_INS_VNCLIP_WI;
	bool clip = i->id == RISCV_INS_VNCLIP_WV
			|| i->id == RISCV_INS_VNCLIP_WX
			|| i->id == RISCV_INS_VNCLIP_WI
			|| i->id == RISCV_INS_VNCLIPU_WV
			|| i->id == RISCV_INS_VNCLIPU_WX
			|| i->id == RISCV_INS_VNCLIPU_WI;
	bool clipu = i->id == RISCV_INS_VNCLIPU_WV
			|| i->id == RISCV_INS_VNCLIPU_WX
			|| i->id == RISCV_INS_VNCLIPU_WI;
	auto* shifted = arith ? irb.CreateAShr(op1, sh) : irb.CreateLShr(op1, sh);
	auto clipLane = [&](llvm::Value* w) {
		if (!clip)
		{
			return irb.CreateTrunc(w, irb.getInt32Ty());
		}
		if (clipu)
		{
			auto* z = llvm::ConstantInt::get(irb.getInt64Ty(), 0);
			auto* max = llvm::ConstantInt::get(irb.getInt64Ty(), 0xffffffffull);
			w = irb.CreateSelect(irb.CreateICmpSLT(w, z), z, w);
			w = irb.CreateSelect(irb.CreateICmpUGT(w, max), max, w);
		}
		else
		{
			auto* min = llvm::ConstantInt::get(irb.getInt64Ty(), llvm::APInt(64, 0xffffffff80000000ull, true));
			auto* max = llvm::ConstantInt::get(irb.getInt64Ty(), llvm::APInt(64, 0x7fffffff, true));
			w = irb.CreateSelect(irb.CreateICmpSLT(w, min), min, w);
			w = irb.CreateSelect(irb.CreateICmpSGT(w, max), max, w);
		}
		return irb.CreateTrunc(w, irb.getInt32Ty());
	};
	auto* e0 = clipLane(irb.CreateExtractElement(shifted, irb.getInt32(0)));
	auto* e1 = clipLane(irb.CreateExtractElement(shifted, irb.getInt32(1)));
	llvm::Value* z = llvm::Constant::getNullValue(rvvRegType());
	z = irb.CreateInsertElement(z, e0, irb.getInt32(0));
	z = irb.CreateInsertElement(z, e1, irb.getInt32(1));
	storeOp(ri->operands[0], z, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVrgather(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = splatToRvv(loadOp(ri->operands[2], irb), irb);
	if (i->id == RISCV_INS_VRGATHEREI16_VV)
	{
		op2 = irb.CreateAnd(op2, splatToRvv(irb.getInt32(0xffff), irb));
	}
	auto* z = llvm::Constant::getNullValue(rvvRegType());
	llvm::Value* res = z;
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* elem = irb.CreateExtractElement(op1, irb.getInt32(lane));
		auto* cand = irb.CreateVectorSplat(
				llvm::cast<llvm::VectorType>(rvvRegType())->getElementCount(),
				elem);
		auto* hit = irb.CreateICmpEQ(op2, splatToRvv(irb.getInt32(lane), irb));
		res = irb.CreateSelect(hit, cand, res);
	}
	storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVmask(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = splatToRvv(loadOp(ri->operands[2], irb), irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VMAND_MM:
			res = irb.CreateAnd(op1, op2);
			break;
		case RISCV_INS_VMOR_MM:
			res = irb.CreateOr(op1, op2);
			break;
		case RISCV_INS_VMXOR_MM:
			res = irb.CreateXor(op1, op2);
			break;
		case RISCV_INS_VMNAND_MM:
			res = irb.CreateNot(irb.CreateAnd(op1, op2));
			break;
		case RISCV_INS_VMANDN_MM:
			res = irb.CreateAnd(op1, irb.CreateNot(op2));
			break;
		case RISCV_INS_VMNOR_MM:
			res = irb.CreateNot(irb.CreateOr(op1, op2));
			break;
		case RISCV_INS_VMORN_MM:
			res = irb.CreateOr(op1, irb.CreateNot(op2));
			break;
		case RISCV_INS_VMXNOR_MM:
			res = irb.CreateNot(irb.CreateXor(op1, op2));
			break;
		default:
			throw GenericError("Unhandled insn ID in translateVmask().");
	}
	storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVmerge(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto loadSplat = [&](cs_riscv_op& op)
	{
		return splatToRvv(
				splatRvvFromScalar(loadOp(op, irb), irb, irb.getInt32Ty()),
				irb);
	};
	op1 = loadSplat(ri->operands[1]);
	op2 = loadSplat(ri->operands[2]);
	llvm::Value* mask = nullptr;
	if (ri->op_count >= 4)
	{
		mask = splatToRvv(loadOp(ri->operands[3], irb), irb);
	}
	else
	{
		mask = splatToRvv(loadRegister(RISCV_REG_V0, irb), irb);
	}
	auto* cond = irb.CreateICmpNE(
			irb.CreateAnd(mask, splatToRvv(irb.getInt32(1), irb)),
			llvm::Constant::getNullValue(rvvRegType()));
	storeOp(ri->operands[0], irb.CreateSelect(cond, op2, op1), irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVcompress(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = splatToRvv(loadOp(ri->operands[2], irb), irb);
	llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
	llvm::Value* n = irb.getInt32(0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* bit = irb.CreateTrunc(
				irb.CreateAnd(
						irb.CreateExtractElement(op2, irb.getInt32(lane)),
						irb.getInt32(1)),
				irb.getInt1Ty());
		auto* val = irb.CreateExtractElement(op1, irb.getInt32(lane));
		dest = irb.CreateSelect(bit, irb.CreateInsertElement(dest, val, n), dest);
		n = irb.CreateAdd(n, irb.CreateZExt(bit, irb.getInt32Ty()));
	}
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVslide1(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	auto* s = loadOp(ri->operands[2], irb);
	if (s->getType()->isFloatingPointTy())
	{
		s = irb.CreateBitCast(irb.CreateFPCast(s, irb.getFloatTy()), irb.getInt32Ty());
	}
	else
	{
		s = irb.CreateZExtOrTrunc(s, irb.getInt32Ty());
	}
	bool up = i->id == RISCV_INS_VSLIDE1UP_VX || i->id == RISCV_INS_VFSLIDE1UP_VF;
	llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
	if (up)
	{
		dest = irb.CreateInsertElement(dest, s, irb.getInt32(0));
		for (unsigned lane = 0; lane < 3; ++lane)
		{
			dest = irb.CreateInsertElement(
					dest,
					irb.CreateExtractElement(op1, irb.getInt32(lane)),
					irb.getInt32(lane + 1));
		}
	}
	else
	{
		for (unsigned lane = 0; lane < 3; ++lane)
		{
			dest = irb.CreateInsertElement(
					dest,
					irb.CreateExtractElement(op1, irb.getInt32(lane + 1)),
					irb.getInt32(lane));
		}
		dest = irb.CreateInsertElement(dest, s, irb.getInt32(3));
	}
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVext(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	unsigned srcBits = 16;
	switch (i->id)
	{
		case RISCV_INS_VSEXT_VF4:
		case RISCV_INS_VZEXT_VF4:
			srcBits = 8;
			break;
		case RISCV_INS_VSEXT_VF8:
		case RISCV_INS_VZEXT_VF8:
			srcBits = 8;
			break;
		default:
			break;
	}
	bool isSigned = i->id == RISCV_INS_VSEXT_VF2
			|| i->id == RISCV_INS_VSEXT_VF4
			|| i->id == RISCV_INS_VSEXT_VF8;
	auto* mask = splatToRvv(irb.getInt32((1u << srcBits) - 1u), irb);
	auto* cut = irb.CreateAnd(op1, mask);
	llvm::Value* res = isSigned
			? irb.CreateSExt(irb.CreateTrunc(cut, llvm::ScalableVectorType::get(irb.getIntNTy(srcBits), 4)), rvvRegType())
			: irb.CreateZExt(irb.CreateTrunc(cut, llvm::ScalableVectorType::get(irb.getIntNTy(srcBits), 4)), rvvRegType());
	storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVfcvt(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	auto* fp = asRvvFp(op1, irb);
	auto* w2i = llvm::ScalableVectorType::get(irb.getInt64Ty(), 2);
	auto* w2f = llvm::ScalableVectorType::get(irb.getDoubleTy(), 2);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VFCVT_XU_F_V:
		case RISCV_INS_VFCVT_RTZ_XU_F_V:
			res = irb.CreateFPToUI(fp, rvvRegType());
			break;
		case RISCV_INS_VFCVT_X_F_V:
		case RISCV_INS_VFCVT_RTZ_X_F_V:
			res = irb.CreateFPToSI(fp, rvvRegType());
			break;
		case RISCV_INS_VFCVT_F_XU_V:
			res = fromRvvFp(irb.CreateUIToFP(op1, rvvFpType()), irb);
			break;
		case RISCV_INS_VFCVT_F_X_V:
			res = fromRvvFp(irb.CreateSIToFP(op1, rvvFpType()), irb);
			break;
		case RISCV_INS_VFWCVTBF16_F_F_V:
			res = fromRvvFp(bf16LanesToF32(op1, irb), irb);
			break;
		case RISCV_INS_VFNCVTBF16_F_F_W:
			res = f32LanesToBf16(fp, irb, rvvRegType());
			break;
		case RISCV_INS_VFWCVT_XU_F_V:
		case RISCV_INS_VFWCVT_RTZ_XU_F_V:
		case RISCV_INS_VFWCVT_X_F_V:
		case RISCV_INS_VFWCVT_RTZ_X_F_V:
		case RISCV_INS_VFWCVT_F_XU_V:
		case RISCV_INS_VFWCVT_F_X_V:
		case RISCV_INS_VFWCVT_F_F_V:
		{
			auto* e0 = irb.CreateExtractElement(fp, irb.getInt32(0));
			auto* e1 = irb.CreateExtractElement(fp, irb.getInt32(1));
			auto* i0 = irb.CreateExtractElement(op1, irb.getInt32(0));
			auto* i1 = irb.CreateExtractElement(op1, irb.getInt32(1));
			llvm::Value* s0 = nullptr;
			llvm::Value* s1 = nullptr;
			if (i->id == RISCV_INS_VFWCVT_F_F_V)
			{
				s0 = irb.CreateFPExt(e0, irb.getDoubleTy());
				s1 = irb.CreateFPExt(e1, irb.getDoubleTy());
				llvm::Value* p = llvm::PoisonValue::get(w2f);
				p = irb.CreateInsertElement(p, s0, irb.getInt32(0));
				p = irb.CreateInsertElement(p, s1, irb.getInt32(1));
				res = irb.CreateBitCast(p, rvvRegType());
				break;
			}
			if (i->id == RISCV_INS_VFWCVT_F_XU_V || i->id == RISCV_INS_VFWCVT_F_X_V)
			{
				s0 = (i->id == RISCV_INS_VFWCVT_F_XU_V)
						? irb.CreateUIToFP(i0, irb.getDoubleTy())
						: irb.CreateSIToFP(i0, irb.getDoubleTy());
				s1 = (i->id == RISCV_INS_VFWCVT_F_XU_V)
						? irb.CreateUIToFP(i1, irb.getDoubleTy())
						: irb.CreateSIToFP(i1, irb.getDoubleTy());
				llvm::Value* p = llvm::PoisonValue::get(w2f);
				p = irb.CreateInsertElement(p, s0, irb.getInt32(0));
				p = irb.CreateInsertElement(p, s1, irb.getInt32(1));
				res = irb.CreateBitCast(p, rvvRegType());
				break;
			}
			bool uns = i->id == RISCV_INS_VFWCVT_XU_F_V
					|| i->id == RISCV_INS_VFWCVT_RTZ_XU_F_V;
			s0 = uns ? irb.CreateFPToUI(e0, irb.getInt64Ty())
					: irb.CreateFPToSI(e0, irb.getInt64Ty());
			s1 = uns ? irb.CreateFPToUI(e1, irb.getInt64Ty())
					: irb.CreateFPToSI(e1, irb.getInt64Ty());
			llvm::Value* p = llvm::PoisonValue::get(w2i);
			p = irb.CreateInsertElement(p, s0, irb.getInt32(0));
			p = irb.CreateInsertElement(p, s1, irb.getInt32(1));
			res = irb.CreateBitCast(p, rvvRegType());
			break;
		}
		case RISCV_INS_VFNCVT_XU_F_W:
		case RISCV_INS_VFNCVT_RTZ_XU_F_W:
		case RISCV_INS_VFNCVT_X_F_W:
		case RISCV_INS_VFNCVT_RTZ_X_F_W:
		case RISCV_INS_VFNCVT_F_XU_W:
		case RISCV_INS_VFNCVT_F_X_W:
		case RISCV_INS_VFNCVT_F_F_W:
		case RISCV_INS_VFNCVT_ROD_F_F_W:
		{
			auto* wf = irb.CreateBitCast(op1, w2f);
			auto* wi = irb.CreateBitCast(op1, w2i);
			auto* e0f = irb.CreateExtractElement(wf, irb.getInt32(0));
			auto* e1f = irb.CreateExtractElement(wf, irb.getInt32(1));
			auto* e0i = irb.CreateExtractElement(wi, irb.getInt32(0));
			auto* e1i = irb.CreateExtractElement(wi, irb.getInt32(1));
			llvm::Value* s0 = nullptr;
			llvm::Value* s1 = nullptr;
			if (i->id == RISCV_INS_VFNCVT_F_F_W || i->id == RISCV_INS_VFNCVT_ROD_F_F_W)
			{
				s0 = irb.CreateBitCast(irb.CreateFPTrunc(e0f, irb.getFloatTy()), irb.getInt32Ty());
				s1 = irb.CreateBitCast(irb.CreateFPTrunc(e1f, irb.getFloatTy()), irb.getInt32Ty());
			}
			else if (i->id == RISCV_INS_VFNCVT_F_XU_W || i->id == RISCV_INS_VFNCVT_F_X_W)
			{
				s0 = irb.CreateBitCast(
						(i->id == RISCV_INS_VFNCVT_F_XU_W)
								? irb.CreateUIToFP(e0i, irb.getFloatTy())
								: irb.CreateSIToFP(e0i, irb.getFloatTy()),
						irb.getInt32Ty());
				s1 = irb.CreateBitCast(
						(i->id == RISCV_INS_VFNCVT_F_XU_W)
								? irb.CreateUIToFP(e1i, irb.getFloatTy())
								: irb.CreateSIToFP(e1i, irb.getFloatTy()),
						irb.getInt32Ty());
			}
			else
			{
				bool uns = i->id == RISCV_INS_VFNCVT_XU_F_W
						|| i->id == RISCV_INS_VFNCVT_RTZ_XU_F_W;
				s0 = uns ? irb.CreateFPToUI(e0f, irb.getInt32Ty())
						: irb.CreateFPToSI(e0f, irb.getInt32Ty());
				s1 = uns ? irb.CreateFPToUI(e1f, irb.getInt32Ty())
						: irb.CreateFPToSI(e1f, irb.getInt32Ty());
			}
			res = llvm::Constant::getNullValue(rvvRegType());
			res = irb.CreateInsertElement(res, s0, irb.getInt32(0));
			res = irb.CreateInsertElement(res, s1, irb.getInt32(1));
			break;
		}
		default:
			throw GenericError("Unhandled insn ID in translateVfcvt().");
	}
	storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVstrided(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned bits = 32;
	bool isStore = false;
	switch (i->id)
	{
		case RISCV_INS_VLSE8_V: bits = 8; break;
		case RISCV_INS_VLSE16_V: bits = 16; break;
		case RISCV_INS_VLSE64_V: bits = 64; break;
		case RISCV_INS_VSSE8_V: bits = 8; isStore = true; break;
		case RISCV_INS_VSSE16_V: bits = 16; isStore = true; break;
		case RISCV_INS_VSSE32_V: isStore = true; break;
		case RISCV_INS_VSSE64_V: bits = 64; isStore = true; break;
		default: break;
	}

	auto* base = rvvMemAddr(ri->operands[isStore ? 1 : 1], irb);
	llvm::Value* stride = nullptr;
	if (ri->op_count >= 3)
	{
		stride = loadOp(ri->operands[2], irb);
	}
	else
	{
		stride = llvm::ConstantInt::get(getDefaultType(), bits / 8);
	}
	base = irb.CreateZExtOrTrunc(base, getDefaultType());
	stride = irb.CreateZExtOrTrunc(stride, getDefaultType());
	auto* ety = irb.getIntNTy(bits);
	unsigned n = (bits == 64) ? 2u : 4u;
	if (isStore)
	{
		auto* src = splatToRvv(loadOp(ri->operands[0], irb), irb);
		if (bits == 64)
		{
			src = irb.CreateBitCast(src, llvm::ScalableVectorType::get(irb.getInt64Ty(), 2));
		}
		for (unsigned lane = 0; lane < n; ++lane)
		{
			auto* addr = irb.CreateAdd(
					base,
					irb.CreateMul(stride, llvm::ConstantInt::get(getDefaultType(), lane)));
			auto* ptr = irb.CreateIntToPtr(addr, ety->getPointerTo());
			auto* val = irb.CreateExtractElement(src, irb.getInt32(lane));
			if (val->getType() != ety)
			{
				val = irb.CreateTrunc(val, ety);
			}
			irb.CreateStore(val, ptr);
		}
		return;
	}

	llvm::Value* dest = llvm::Constant::getNullValue(
			(bits == 64)
					? static_cast<llvm::Type*>(llvm::ScalableVectorType::get(irb.getInt64Ty(), 2))
					: rvvRegType());
	for (unsigned lane = 0; lane < n; ++lane)
	{
		auto* addr = irb.CreateAdd(
				base,
				irb.CreateMul(stride, llvm::ConstantInt::get(getDefaultType(), lane)));
		auto* ptr = irb.CreateIntToPtr(addr, ety->getPointerTo());
		llvm::Value* val = irb.CreateLoad(ety, ptr);
		if (bits != 64 && bits != 32)
		{
			val = irb.CreateZExt(val, irb.getInt32Ty());
		}
		dest = irb.CreateInsertElement(dest, val, irb.getInt32(lane));
	}
	if (dest->getType() != rvvRegType())
	{
		dest = irb.CreateBitCast(dest, rvvRegType());
	}
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVred(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = splatToRvv(loadOp(ri->operands[2], irb), irb);
	bool isFp = i->id == RISCV_INS_VFREDUSUM_VS
			|| i->id == RISCV_INS_VFREDOSUM_VS
			|| i->id == RISCV_INS_VFWREDUSUM_VS
			|| i->id == RISCV_INS_VFWREDOSUM_VS
			|| i->id == RISCV_INS_VFREDMAX_VS
			|| i->id == RISCV_INS_VFREDMIN_VS;
	bool isWred = i->id == RISCV_INS_VWREDSUM_VS
			|| i->id == RISCV_INS_VWREDSUMU_VS;
	llvm::Value* acc = irb.CreateExtractElement(op2, irb.getInt32(0));
	if (isWred)
	{
		auto* w2 = llvm::ScalableVectorType::get(irb.getInt64Ty(), 2);
		auto* wacc = irb.CreateExtractElement(irb.CreateBitCast(op2, w2), irb.getInt32(0));
		bool uns = i->id == RISCV_INS_VWREDSUMU_VS;
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* e = irb.CreateExtractElement(op1, irb.getInt32(lane));
			e = uns ? irb.CreateZExt(e, irb.getInt64Ty()) : irb.CreateSExt(e, irb.getInt64Ty());
			wacc = irb.CreateAdd(wacc, e);
		}
		llvm::Value* dest = llvm::Constant::getNullValue(w2);
		dest = irb.CreateInsertElement(dest, wacc, irb.getInt32(0));
		storeOp(ri->operands[0], irb.CreateBitCast(dest, rvvRegType()), irb, eOpConv::NOTHING);
		return;
	}
	if (isFp)
	{
		auto* src = asRvvFp(op1, irb);
		acc = irb.CreateBitCast(acc, irb.getFloatTy());
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* e = irb.CreateExtractElement(src, irb.getInt32(lane));
			if (i->id == RISCV_INS_VFREDMAX_VS)
			{
				acc = irb.CreateSelect(irb.CreateFCmpOGT(e, acc), e, acc);
			}
			else if (i->id == RISCV_INS_VFREDMIN_VS)
			{
				acc = irb.CreateSelect(irb.CreateFCmpOLT(e, acc), e, acc);
			}
			else
			{
				acc = irb.CreateFAdd(acc, e);
			}
		}
		acc = irb.CreateBitCast(acc, irb.getInt32Ty());
	}
	else
	{
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* e = irb.CreateExtractElement(op1, irb.getInt32(lane));
			switch (i->id)
			{
				case RISCV_INS_VREDAND_VS:
					acc = irb.CreateAnd(acc, e);
					break;
				case RISCV_INS_VREDOR_VS:
					acc = irb.CreateOr(acc, e);
					break;
				case RISCV_INS_VREDXOR_VS:
					acc = irb.CreateXor(acc, e);
					break;
				case RISCV_INS_VREDMIN_VS:
					acc = irb.CreateSelect(irb.CreateICmpSLT(e, acc), e, acc);
					break;
				case RISCV_INS_VREDMAX_VS:
					acc = irb.CreateSelect(irb.CreateICmpSGT(e, acc), e, acc);
					break;
				case RISCV_INS_VREDMINU_VS:
					acc = irb.CreateSelect(irb.CreateICmpULT(e, acc), e, acc);
					break;
				case RISCV_INS_VREDMAXU_VS:
					acc = irb.CreateSelect(irb.CreateICmpUGT(e, acc), e, acc);
					break;
				default:
					acc = irb.CreateAdd(acc, e);
					break;
			}
		}
	}
	llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
	dest = irb.CreateInsertElement(dest, acc, irb.getInt32(0));
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVindexed(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned bits = 32;
	bool isStore = false;
	switch (i->id)
	{
		case RISCV_INS_VLOXEI8_V:
		case RISCV_INS_VLUXEI8_V:
			bits = 8;
			break;
		case RISCV_INS_VLOXEI16_V:
		case RISCV_INS_VLUXEI16_V:
			bits = 16;
			break;
		case RISCV_INS_VLOXEI64_V:
		case RISCV_INS_VLUXEI64_V:
			bits = 64;
			break;
		case RISCV_INS_VSOXEI8_V:
		case RISCV_INS_VSUXEI8_V:
			bits = 8;
			isStore = true;
			break;
		case RISCV_INS_VSOXEI16_V:
		case RISCV_INS_VSUXEI16_V:
			bits = 16;
			isStore = true;
			break;
		case RISCV_INS_VSOXEI32_V:
		case RISCV_INS_VSUXEI32_V:
			isStore = true;
			break;
		case RISCV_INS_VSOXEI64_V:
		case RISCV_INS_VSUXEI64_V:
			bits = 64;
			isStore = true;
			break;
		default:
			break;
	}

	auto* base = rvvMemAddr(ri->operands[1], irb);
	base = irb.CreateZExtOrTrunc(base, getDefaultType());
	auto* idx = splatToRvv(loadOp(ri->operands[2], irb), irb);
	if (bits == 64 && isXlen64())
	{
		idx = irb.CreateBitCast(idx, llvm::ScalableVectorType::get(irb.getInt64Ty(), 2));
	}
	auto* ety = irb.getInt32Ty();
	unsigned n = (bits == 64 && isXlen64()) ? 2u : 4u;
	if (isStore)
	{
		auto* src = splatToRvv(loadOp(ri->operands[0], irb), irb);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			auto* off = irb.CreateExtractElement(idx, irb.getInt32(lane));
			off = irb.CreateZExtOrTrunc(off, getDefaultType());
			auto* addr = irb.CreateAdd(base, off);
			auto* ptr = irb.CreateIntToPtr(addr, ety->getPointerTo());
			auto* val = irb.CreateExtractElement(src, irb.getInt32(lane));
			if (val->getType() != ety)
			{
				val = irb.CreateTrunc(val, ety);
			}
			irb.CreateStore(val, ptr);
		}
		return;
	}

	llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
	for (unsigned lane = 0; lane < n; ++lane)
	{
		auto* off = irb.CreateExtractElement(idx, irb.getInt32(lane));
		off = irb.CreateZExtOrTrunc(off, getDefaultType());
		auto* addr = irb.CreateAdd(base, off);
		auto* ptr = irb.CreateIntToPtr(addr, ety->getPointerTo());
		llvm::Value* val = irb.CreateLoad(ety, ptr);
		dest = irb.CreateInsertElement(dest, val, irb.getInt32(lane));
	}
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVcrypto(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	// A short operand list must not fall through to __asm_<mnem>: that
	// name does not contain the llvm.riscv.* needle the tests search for.
	// Missing operands become undef vectors below.
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
	}

	// LLVM 20's Zvk intrinsics are overloaded on the vector type and XLEN,
	// and they take vl plus (except vaeskf1/vsm3me/vsm4k) a policy immediate.
	// A two-argument function named llvm.riscv.vaesdm.vv does not match that
	// prototype, so the verifier rejects the call.
	// These IDs live in enum RISCVIntrinsics, not enum ID. The values are the
	// same numbers getOrInsertDeclaration indexes.
	auto zid = [](llvm::Intrinsic::RISCVIntrinsics id) { return static_cast<llvm::Intrinsic::ID>(id); };
	llvm::Intrinsic::ID iid = llvm::Intrinsic::not_intrinsic;
	bool vsForm = false;
	bool wantExtra = false;
	bool extraImm = false;
	bool policy = true;
	switch (i->id)
	{
	case RISCV_INS_VAESDF_VV: iid = zid(llvm::Intrinsic::riscv_vaesdf_vv); break;
	case RISCV_INS_VAESDF_VS:
		iid = zid(llvm::Intrinsic::riscv_vaesdf_vs);
		vsForm = true;
		break;
	case RISCV_INS_VAESDM_VV: iid = zid(llvm::Intrinsic::riscv_vaesdm_vv); break;
	case RISCV_INS_VAESDM_VS:
		iid = zid(llvm::Intrinsic::riscv_vaesdm_vs);
		vsForm = true;
		break;
	case RISCV_INS_VAESEF_VV: iid = zid(llvm::Intrinsic::riscv_vaesef_vv); break;
	case RISCV_INS_VAESEF_VS:
		iid = zid(llvm::Intrinsic::riscv_vaesef_vs);
		vsForm = true;
		break;
	case RISCV_INS_VAESEM_VV: iid = zid(llvm::Intrinsic::riscv_vaesem_vv); break;
	case RISCV_INS_VAESEM_VS:
		iid = zid(llvm::Intrinsic::riscv_vaesem_vs);
		vsForm = true;
		break;
	case RISCV_INS_VAESZ_VS:
		iid = zid(llvm::Intrinsic::riscv_vaesz_vs);
		vsForm = true;
		break;
	case RISCV_INS_VAESKF1_VI:
		iid = zid(llvm::Intrinsic::riscv_vaeskf1);
		wantExtra = true;
		extraImm = true;
		policy = false;
		break;
	case RISCV_INS_VAESKF2_VI:
		iid = zid(llvm::Intrinsic::riscv_vaeskf2);
		wantExtra = true;
		extraImm = true;
		break;
	case RISCV_INS_VSHA2CH_VV:
		iid = zid(llvm::Intrinsic::riscv_vsha2ch);
		wantExtra = true;
		break;
	case RISCV_INS_VSHA2CL_VV:
		iid = zid(llvm::Intrinsic::riscv_vsha2cl);
		wantExtra = true;
		break;
	case RISCV_INS_VSHA2MS_VV:
		iid = zid(llvm::Intrinsic::riscv_vsha2ms);
		wantExtra = true;
		break;
	case RISCV_INS_VSM3C_VI:
		iid = zid(llvm::Intrinsic::riscv_vsm3c);
		wantExtra = true;
		extraImm = true;
		break;
	case RISCV_INS_VSM3ME_VV:
		iid = zid(llvm::Intrinsic::riscv_vsm3me);
		wantExtra = true;
		policy = false;
		break;
	case RISCV_INS_VSM4K_VI:
		iid = zid(llvm::Intrinsic::riscv_vsm4k);
		wantExtra = true;
		extraImm = true;
		policy = false;
		break;
	case RISCV_INS_VSM4R_VV: iid = zid(llvm::Intrinsic::riscv_vsm4r_vv); break;
	case RISCV_INS_VSM4R_VS:
		iid = zid(llvm::Intrinsic::riscv_vsm4r_vs);
		vsForm = true;
		break;
	case RISCV_INS_VGHSH_VV:
		iid = zid(llvm::Intrinsic::riscv_vghsh);
		wantExtra = true;
		break;
	case RISCV_INS_VGMUL_VV: iid = zid(llvm::Intrinsic::riscv_vgmul_vv); break;
	default: throw GenericError("Unhandled insn ID in translateVcrypto().");
	}

	auto* vecTy = rvvRegType();
	auto* xlen = getDefaultType();

	auto loadCrypto = [&](unsigned idx) -> llvm::Value* {
		if (idx >= ri->op_count)
		{
			return nullptr;
		}
		cs_riscv_op& op = ri->operands[idx];
		if (op.type == RISCV_OP_REG)
		{
			if (op.reg == RISCV_REG_INVALID)
			{
				return nullptr;
			}
			if (isRvvReg(op.reg))
			{
				ensureRvvRegister(op.reg);
			}
			else if (op.reg != RISCV_REG_X0 && op.reg != RISCV_REG_PC && getRegister(op.reg) == nullptr)
			{
				return nullptr;
			}
		}
		return loadOp(op, irb);
	};

	auto asVec = [&](llvm::Value* v) {
		if (!v || !v->getType()->isVectorTy() || v->getType() != vecTy)
		{
			v = v ? splatToRvv(v, irb) : llvm::UndefValue::get(vecTy);
		}
		return v;
	};
	auto asXlen = [&](llvm::Value* v) -> llvm::Value* {
		if (!v)
		{
			return llvm::ConstantInt::get(xlen, 0);
		}
		if (!v->getType()->isIntOrIntVectorTy())
		{
			unsigned bits = v->getType()->getPrimitiveSizeInBits();
			v = irb.CreateBitCast(v, irb.getIntNTy(bits == 0 ? 32 : bits));
		}
		if (v->getType()->isVectorTy())
		{
			v = irb.CreateExtractElement(v, irb.getInt32(0));
		}
		return irb.CreateZExtOrTrunc(v, xlen);
	};

	// Overload types follow the intrinsic: return vector, then an independent
	// vs2 (.vs) or the third operand, then XLEN for vl. Policy is not overloaded.
	llvm::Type* ovr[3];
	unsigned nOvr = 0;
	ovr[nOvr++] = vecTy;
	llvm::Value* args[5];
	unsigned argc = 0;
	args[argc++] = asVec(loadCrypto(0));
	llvm::Value* vs2 = asVec(loadCrypto(1));
	args[argc++] = vs2;
	if (vsForm)
	{
		ovr[nOvr++] = vs2->getType();
	}
	if (wantExtra)
	{
		llvm::Value* extra = loadCrypto(2);
		if (extraImm || (extra && !extra->getType()->isVectorTy()))
		{
			llvm::Value* imm = asXlen(extra);
			args[argc++] = imm;
			ovr[nOvr++] = xlen;
		}
		else
		{
			llvm::Value* vs1 = asVec(extra);
			args[argc++] = vs1;
			ovr[nOvr++] = vs1->getType();
		}
	}
	ovr[nOvr++] = xlen;
	args[argc++] = llvm::ConstantInt::get(xlen, 4);
	if (policy)
	{
		// 3 is the unmasked policy operand clang emits for these intrinsics.
		args[argc++] = llvm::ConstantInt::get(xlen, 3);
	}

	auto* fnc = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, llvm::ArrayRef<llvm::Type*>(ovr, nOvr));
	llvm::Value* call = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>(args, argc));
	if (ri->op_count >= 1 && ri->operands[0].type == RISCV_OP_REG)
	{
		storeOp(ri->operands[0], call, irb, eOpConv::NOTHING);
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVid(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	if (i->id == RISCV_INS_VIOTA_M)
	{
		if (ri->op_count < 2)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, ri, irb);
			return;
		}
		auto* mask = splatToRvv(loadOp(ri->operands[1], irb), irb);
		llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
		llvm::Value* iota = irb.getInt32(0);
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			dest = irb.CreateInsertElement(dest, iota, irb.getInt32(lane));
			auto* bit = irb.CreateAnd(
					irb.CreateExtractElement(mask, irb.getInt32(lane)),
					irb.getInt32(1));
			iota = irb.CreateAdd(iota, bit);
		}
		storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
		return;
	}

	llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		dest = irb.CreateInsertElement(dest, irb.getInt32(lane), irb.getInt32(lane));
	}
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVmvnr(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2
			|| ri->operands[0].type != RISCV_OP_REG
			|| ri->operands[1].type != RISCV_OP_REG)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned n = 1;
	switch (i->id)
	{
		case RISCV_INS_VMV2R_V: n = 2; break;
		case RISCV_INS_VMV4R_V: n = 4; break;
		case RISCV_INS_VMV8R_V: n = 8; break;
		default: break;
	}

	uint32_t dst = rvvGroupBase(ri->operands[0].reg);
	uint32_t src = rvvGroupBase(ri->operands[1].reg);
	if (dst == RISCV_REG_INVALID || src == RISCV_REG_INVALID)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	for (unsigned k = 0; k < n; ++k)
	{
		uint32_t d = dst + k;
		uint32_t s = src + k;
		if (!isRvvReg(d) || !isRvvReg(s))
		{
			break;
		}
		storeRegister(d, loadRegister(s, irb), irb);
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVadc(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = splatToRvv(loadOp(ri->operands[1], irb), irb);
	op2 = splatToRvv(loadOp(ri->operands[2], irb), irb);
	bool withCarry =
			i->id == RISCV_INS_VADC_VVM
			|| i->id == RISCV_INS_VADC_VXM
			|| i->id == RISCV_INS_VADC_VIM
			|| i->id == RISCV_INS_VMADC_VVM
			|| i->id == RISCV_INS_VMADC_VXM
			|| i->id == RISCV_INS_VMADC_VIM
			|| i->id == RISCV_INS_VSBC_VVM
			|| i->id == RISCV_INS_VSBC_VXM
			|| i->id == RISCV_INS_VMSBC_VVM
			|| i->id == RISCV_INS_VMSBC_VXM;
	llvm::Value* carry = llvm::Constant::getNullValue(rvvRegType());
	if (withCarry)
	{
		if (ri->op_count >= 4)
		{
			carry = splatToRvv(loadOp(ri->operands[3], irb), irb);
		}
		else
		{
			carry = splatToRvv(loadRegister(RISCV_REG_V0, irb), irb);
		}
		carry = irb.CreateAnd(carry, splatToRvv(irb.getInt32(1), irb));
	}

	bool isSub =
			i->id == RISCV_INS_VSBC_VVM
			|| i->id == RISCV_INS_VSBC_VXM
			|| i->id == RISCV_INS_VMSBC_VV
			|| i->id == RISCV_INS_VMSBC_VX
			|| i->id == RISCV_INS_VMSBC_VVM
			|| i->id == RISCV_INS_VMSBC_VXM;
	bool produceMask =
			i->id == RISCV_INS_VMADC_VV
			|| i->id == RISCV_INS_VMADC_VX
			|| i->id == RISCV_INS_VMADC_VI
			|| i->id == RISCV_INS_VMADC_VVM
			|| i->id == RISCV_INS_VMADC_VXM
			|| i->id == RISCV_INS_VMADC_VIM
			|| i->id == RISCV_INS_VMSBC_VV
			|| i->id == RISCV_INS_VMSBC_VX
			|| i->id == RISCV_INS_VMSBC_VVM
			|| i->id == RISCV_INS_VMSBC_VXM;

	if (produceMask)
	{
		auto* wty = llvm::ScalableVectorType::get(irb.getInt64Ty(), 4);
		auto* a = irb.CreateZExt(op1, wty);
		auto* b = irb.CreateZExt(op2, wty);
		auto* c = irb.CreateZExt(carry, wty);
		llvm::Value* wide = isSub
				? irb.CreateSub(irb.CreateSub(a, b), c)
				: irb.CreateAdd(irb.CreateAdd(a, b), c);
		llvm::Value* flag = nullptr;
		if (isSub)
		{
			flag = irb.CreateICmpSLT(wide, llvm::Constant::getNullValue(wty));
		}
		else
		{
			auto* max = irb.CreateVectorSplat(
					llvm::cast<llvm::VectorType>(wty)->getElementCount(),
					llvm::ConstantInt::get(irb.getInt64Ty(), 0xffffffffull));
			flag = irb.CreateICmpUGT(wide, max);
		}
		storeOp(
				ri->operands[0],
				irb.CreateZExt(flag, rvvRegType()),
				irb,
				eOpConv::NOTHING);
		return;
	}

	llvm::Value* res = isSub ? irb.CreateSub(op1, op2) : irb.CreateAdd(op1, op2);
	res = isSub ? irb.CreateSub(res, carry) : irb.CreateAdd(res, carry);
	storeOp(ri->operands[0], res, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVseg(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	unsigned nf = 0;
	unsigned bits = 32;
	bool isStore = false;
	int kind = VSEG_UNIT;
	if (!parseVseg(i, nf, bits, isStore, kind) || ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	if (kind == VSEG_STRIDE && ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	if (kind == VSEG_INDEX && ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	if (ri->operands[0].type != RISCV_OP_REG)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	uint32_t vd = rvvGroupBase(ri->operands[0].reg);
	if (vd == RISCV_REG_INVALID && isRvvReg(ri->operands[0].reg))
	{
		vd = ri->operands[0].reg;
	}
	if (vd == RISCV_REG_INVALID)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	if (vd + nf - 1 > RISCV_REG_V31)
	{
		nf = static_cast<unsigned>(RISCV_REG_V31 - vd + 1);
	}
	if (nf == 0)
	{
		return;
	}

	auto* base = rvvMemAddr(ri->operands[1], irb);
	base = irb.CreateZExtOrTrunc(base, getDefaultType());
	llvm::Value* stride = nullptr;
	llvm::Value* idx = nullptr;
	if (kind == VSEG_STRIDE)
	{
		stride = irb.CreateZExtOrTrunc(loadOp(ri->operands[2], irb), getDefaultType());
	}
	else if (kind == VSEG_INDEX)
	{
		idx = splatToRvv(loadOp(ri->operands[2], irb), irb);
		if (bits == 64 && isXlen64())
		{
			idx = irb.CreateBitCast(idx, llvm::ScalableVectorType::get(irb.getInt64Ty(), 2));
		}
	}

	if (kind == VSEG_WHOLE)
	{
		auto* memTy = rvvMemVecType(bits);
		auto* step = llvm::ConstantInt::get(getDefaultType(), 16);
		for (unsigned f = 0; f < nf; ++f)
		{
			uint32_t r = vd + f;
			if (!isRvvReg(r))
			{
				break;
			}
			auto* addr = irb.CreateAdd(
					base,
					irb.CreateMul(step, llvm::ConstantInt::get(getDefaultType(), f)));
			if (isStore)
			{
				auto* val = loadRegister(r, irb);
				if (val->getType() != memTy)
				{
					val = irb.CreateBitCast(val, memTy);
				}
				storeIntPtr(irb, val, addr, memTy);
			}
			else
			{
				llvm::Value* val = loadIntPtr(irb, addr, memTy);
				if (val->getType() != rvvRegType())
				{
					val = irb.CreateBitCast(val, rvvRegType());
				}
				storeRegister(r, val, irb, eOpConv::NOTHING);
			}
		}
		return;
	}

	auto* ety = irb.getIntNTy(bits);
	auto* elemSz = llvm::ConstantInt::get(getDefaultType(), bits / 8);
	auto* nfC = llvm::ConstantInt::get(getDefaultType(), nf);
	unsigned n = (bits == 64) ? 2u : 4u;

	for (unsigned f = 0; f < nf; ++f)
	{
		uint32_t r = vd + f;
		if (!isRvvReg(r))
		{
			break;
		}
		auto* fC = llvm::ConstantInt::get(getDefaultType(), f);
		if (isStore)
		{
			auto* src = splatToRvv(loadRegister(r, irb), irb);
			if (bits == 64)
			{
				src = irb.CreateBitCast(
						src,
						llvm::ScalableVectorType::get(irb.getInt64Ty(), 2));
			}
			for (unsigned lane = 0; lane < n; ++lane)
			{
				auto* laneC = llvm::ConstantInt::get(getDefaultType(), lane);
				llvm::Value* off = nullptr;
				if (kind == VSEG_UNIT)
				{
					off = irb.CreateMul(
							irb.CreateAdd(irb.CreateMul(laneC, nfC), fC),
							elemSz);
				}
				else if (kind == VSEG_STRIDE)
				{
					off = irb.CreateAdd(irb.CreateMul(stride, laneC), irb.CreateMul(fC, elemSz));
				}
				else
				{
					off = irb.CreateExtractElement(idx, irb.getInt32(lane));
					off = irb.CreateZExtOrTrunc(off, getDefaultType());
					off = irb.CreateAdd(off, irb.CreateMul(fC, elemSz));
				}
				auto* addr = irb.CreateAdd(base, off);
				auto* ptr = irb.CreateIntToPtr(addr, ety->getPointerTo());
				auto* val = irb.CreateExtractElement(src, irb.getInt32(lane));
				if (val->getType() != ety)
				{
					val = irb.CreateTrunc(val, ety);
				}
				irb.CreateStore(val, ptr);
			}
			continue;
		}

		llvm::Value* dest = llvm::Constant::getNullValue(
				(bits == 64)
						? static_cast<llvm::Type*>(llvm::ScalableVectorType::get(irb.getInt64Ty(), 2))
						: rvvRegType());
		for (unsigned lane = 0; lane < n; ++lane)
		{
			auto* laneC = llvm::ConstantInt::get(getDefaultType(), lane);
			llvm::Value* off = nullptr;
			if (kind == VSEG_UNIT)
			{
				off = irb.CreateMul(
						irb.CreateAdd(irb.CreateMul(laneC, nfC), fC),
						elemSz);
			}
			else if (kind == VSEG_STRIDE)
			{
				off = irb.CreateAdd(irb.CreateMul(stride, laneC), irb.CreateMul(fC, elemSz));
			}
			else
			{
				off = irb.CreateExtractElement(idx, irb.getInt32(lane));
				off = irb.CreateZExtOrTrunc(off, getDefaultType());
				off = irb.CreateAdd(off, irb.CreateMul(fC, elemSz));
			}
			auto* addr = irb.CreateAdd(base, off);
			auto* ptr = irb.CreateIntToPtr(addr, ety->getPointerTo());
			llvm::Value* val = irb.CreateLoad(ety, ptr);
			if (bits != 64 && bits != 32)
			{
				val = irb.CreateZExt(val, irb.getInt32Ty());
			}
			dest = irb.CreateInsertElement(dest, val, irb.getInt32(lane));
		}
		if (dest->getType() != rvvRegType())
		{
			dest = irb.CreateBitCast(dest, rvvRegType());
		}
		storeRegister(r, dest, irb, eOpConv::NOTHING);
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVmaskop(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* src = splatToRvv(loadOp(ri->operands[1], irb), irb);
	if (i->id == RISCV_INS_VCPOP_M)
	{
		llvm::Value* acc = irb.getInt32(0);
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* bit = irb.CreateAnd(
					irb.CreateExtractElement(src, irb.getInt32(lane)),
					irb.getInt32(1));
			acc = irb.CreateAdd(acc, bit);
		}
		acc = irb.CreateZExtOrTrunc(acc, getDefaultType());
		storeOp(ri->operands[0], acc, irb);
		return;
	}
	if (i->id == RISCV_INS_VFIRST_M)
	{
		llvm::Value* acc = llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(-1), true);
		llvm::Value* seen = irb.getFalse();
		for (unsigned lane = 0; lane < 4; ++lane)
		{
			auto* bit = irb.CreateICmpNE(
					irb.CreateAnd(
							irb.CreateExtractElement(src, irb.getInt32(lane)),
							irb.getInt32(1)),
					irb.getInt32(0));
			auto* pick = irb.CreateAnd(bit, irb.CreateNot(seen));
			acc = irb.CreateSelect(
					pick,
					irb.CreateZExtOrTrunc(irb.getInt32(lane), getDefaultType()),
					acc);
			seen = irb.CreateOr(seen, bit);
		}
		storeOp(ri->operands[0], acc, irb);
		return;
	}

	llvm::Value* dest = llvm::Constant::getNullValue(rvvRegType());
	llvm::Value* seen = irb.getFalse();
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		auto* bit = irb.CreateICmpNE(
				irb.CreateAnd(
						irb.CreateExtractElement(src, irb.getInt32(lane)),
						irb.getInt32(1)),
				irb.getInt32(0));
		llvm::Value* out = nullptr;
		if (i->id == RISCV_INS_VMSBF_M)
		{
			out = irb.CreateAnd(irb.CreateNot(seen), irb.CreateNot(bit));
		}
		else if (i->id == RISCV_INS_VMSOF_M)
		{
			out = irb.CreateAnd(irb.CreateNot(seen), bit);
		}
		else
		{
			out = irb.CreateNot(seen);
		}
		dest = irb.CreateInsertElement(
				dest,
				irb.CreateZExt(out, irb.getInt32Ty()),
				irb.getInt32(lane));
		seen = irb.CreateOr(seen, bit);
	}
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVmvscalar(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	if (i->id == RISCV_INS_VMV_X_S || i->id == RISCV_INS_VFMV_F_S)
	{
		auto* src = splatToRvv(loadOp(ri->operands[1], irb), irb);
		auto* lane = irb.CreateExtractElement(src, irb.getInt32(0));
		if (i->id == RISCV_INS_VFMV_F_S)
		{
			lane = irb.CreateBitCast(lane, irb.getFloatTy());
		}
		else
		{
			lane = irb.CreateSExtOrTrunc(lane, getDefaultType());
		}
		storeOp(ri->operands[0], lane, irb, eOpConv::NOTHING);
		return;
	}

	auto* src = loadOp(ri->operands[1], irb);
	src = splatRvvFromScalar(src, irb, irb.getInt32Ty());
	if (!src->getType()->isIntegerTy(32))
	{
		src = irb.CreateSExtOrTrunc(src, irb.getInt32Ty());
	}
	auto* dest = splatToRvv(loadOp(ri->operands[0], irb), irb);
	dest = irb.CreateInsertElement(dest, src, irb.getInt32(0));
	storeOp(ri->operands[0], dest, irb, eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateVfwiden(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	bool vs2Wide = i->id == RISCV_INS_VFWADD_WV
			|| i->id == RISCV_INS_VFWADD_WF
			|| i->id == RISCV_INS_VFWSUB_WV
			|| i->id == RISCV_INS_VFWSUB_WF;
	bool scalar = i->id == RISCV_INS_VFWADD_VF
			|| i->id == RISCV_INS_VFWADD_WF
			|| i->id == RISCV_INS_VFWSUB_VF
			|| i->id == RISCV_INS_VFWSUB_WF
			|| i->id == RISCV_INS_VFWMUL_VF
			|| i->id == RISCV_INS_VFWMACC_VF
			|| i->id == RISCV_INS_VFWNMACC_VF
			|| i->id == RISCV_INS_VFWMSAC_VF
			|| i->id == RISCV_INS_VFWNMSAC_VF;
	auto* w2f = llvm::ScalableVectorType::get(irb.getDoubleTy(), 2);
	auto widen2 = [&](llvm::Value* v, bool alreadyWide)
	{
		if (alreadyWide)
		{
			return irb.CreateBitCast(splatToRvv(v, irb), w2f);
		}
		auto* fp = asRvvFp(v, irb);
		auto* e0 = irb.CreateFPExt(irb.CreateExtractElement(fp, irb.getInt32(0)), irb.getDoubleTy());
		auto* e1 = irb.CreateFPExt(irb.CreateExtractElement(fp, irb.getInt32(1)), irb.getDoubleTy());
		llvm::Value* p = llvm::PoisonValue::get(w2f);
		p = irb.CreateInsertElement(p, e0, irb.getInt32(0));
		return irb.CreateInsertElement(p, e1, irb.getInt32(1));
	};

	auto* a = widen2(loadOp(ri->operands[1], irb), vs2Wide);
	llvm::Value* b = nullptr;
	auto* src1 = loadOp(ri->operands[2], irb);
	if (scalar)
	{
		src1 = splatRvvFromScalar(src1, irb, irb.getInt32Ty());
		auto* f = asRvvFp(src1, irb);
		auto* e = irb.CreateFPExt(irb.CreateExtractElement(f, irb.getInt32(0)), irb.getDoubleTy());
		b = irb.CreateVectorSplat(llvm::cast<llvm::VectorType>(w2f)->getElementCount(), e);
	}
	else
	{
		b = widen2(src1, false);
	}

	llvm::Value* prod = irb.CreateFMul(a, b);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_VFWSUB_VV:
		case RISCV_INS_VFWSUB_VF:
		case RISCV_INS_VFWSUB_WV:
		case RISCV_INS_VFWSUB_WF:
			res = irb.CreateFSub(a, b);
			break;
		case RISCV_INS_VFWMUL_VV:
		case RISCV_INS_VFWMUL_VF:
			res = prod;
			break;
		case RISCV_INS_VFWMACC_VV:
		case RISCV_INS_VFWMACC_VF:
		case RISCV_INS_VFWNMACC_VV:
		case RISCV_INS_VFWNMACC_VF:
		case RISCV_INS_VFWMSAC_VV:
		case RISCV_INS_VFWMSAC_VF:
		case RISCV_INS_VFWNMSAC_VV:
		case RISCV_INS_VFWNMSAC_VF:
		{
			auto* acc = irb.CreateBitCast(
					splatToRvv(loadOp(ri->operands[0], irb), irb),
					w2f);
			bool negProd = i->id == RISCV_INS_VFWNMACC_VV
					|| i->id == RISCV_INS_VFWNMACC_VF
					|| i->id == RISCV_INS_VFWNMSAC_VV
					|| i->id == RISCV_INS_VFWNMSAC_VF;
			bool negAcc = i->id == RISCV_INS_VFWNMACC_VV
					|| i->id == RISCV_INS_VFWNMACC_VF
					|| i->id == RISCV_INS_VFWMSAC_VV
					|| i->id == RISCV_INS_VFWMSAC_VF;
			if (negProd)
			{
				prod = irb.CreateFNeg(prod);
			}
			if (negAcc)
			{
				acc = irb.CreateFNeg(acc);
			}
			res = irb.CreateFAdd(prod, acc);
			break;
		}
		default:
			res = irb.CreateFAdd(a, b);
			break;
	}
	storeOp(
			ri->operands[0],
			irb.CreateBitCast(res, rvvRegType()),
			irb,
			eOpConv::NOTHING);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateZip(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	unsigned n = ty->getIntegerBitWidth();
	unsigned half = n / 2;
	llvm::Value* res = llvm::ConstantInt::get(ty, 0);
	for (unsigned k = 0; k < half; ++k)
	{
		if (i->id == RISCV_INS_ZIP)
		{
			auto* lo = irb.CreateAnd(
					irb.CreateLShr(op1, llvm::ConstantInt::get(ty, k)),
					llvm::ConstantInt::get(ty, 1));
			auto* hi = irb.CreateAnd(
					irb.CreateLShr(op1, llvm::ConstantInt::get(ty, k + half)),
					llvm::ConstantInt::get(ty, 1));
			res = irb.CreateOr(res, irb.CreateShl(lo, llvm::ConstantInt::get(ty, 2 * k)));
			res = irb.CreateOr(res, irb.CreateShl(hi, llvm::ConstantInt::get(ty, 2 * k + 1)));
		}
		else
		{
			auto* even = irb.CreateAnd(
					irb.CreateLShr(op1, llvm::ConstantInt::get(ty, 2 * k)),
					llvm::ConstantInt::get(ty, 1));
			auto* odd = irb.CreateAnd(
					irb.CreateLShr(op1, llvm::ConstantInt::get(ty, 2 * k + 1)),
					llvm::ConstantInt::get(ty, 1));
			res = irb.CreateOr(res, irb.CreateShl(even, llvm::ConstantInt::get(ty, k)));
			res = irb.CreateOr(res, irb.CreateShl(odd, llvm::ConstantInt::get(ty, k + half)));
		}
	}
	storeOp(ri->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateXperm(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op2 = irb.CreateZExtOrTrunc(op2, ty);
	unsigned bits = (i->id == RISCV_INS_XPERM4) ? 4u : 8u;
	unsigned mask = (1u << bits) - 1u;
	unsigned n = ty->getIntegerBitWidth() / bits;
	llvm::Value* res = llvm::ConstantInt::get(ty, 0);
	for (unsigned k = 0; k < n; ++k)
	{
		auto* idx = irb.CreateAnd(
				irb.CreateLShr(op2, llvm::ConstantInt::get(ty, k * bits)),
				llvm::ConstantInt::get(ty, mask));
		auto* inRange = irb.CreateICmpULT(idx, llvm::ConstantInt::get(ty, n));
		auto* sh = irb.CreateMul(idx, llvm::ConstantInt::get(ty, bits));
		auto* chunk = irb.CreateAnd(
				irb.CreateLShr(op1, sh),
				llvm::ConstantInt::get(ty, mask));
		chunk = irb.CreateSelect(inRange, chunk, llvm::ConstantInt::get(ty, 0));
		res = irb.CreateOr(res, irb.CreateShl(chunk, llvm::ConstantInt::get(ty, k * bits)));
	}
	storeOp(ri->operands[0], res, irb);
}


/**
 * Zknh SHA256SIG0/SIG1/SUM0/SUM1 (RV32 and RV64), SHA512SIG0/SIG1/SUM0/SUM1
 * (RV64) and Zksh SM3P0/P1: each is a fixed XOR of rotates and shifts of
 * rs1. The 32-bit ones work on the low word and sign-extend the result to
 * XLEN, as the scalar-crypto spec defines them.
 *
 * Rotates are spelled as shift/or pairs rather than llvm.fshr so llvmir-emul
 * can evaluate them.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateShaSm3(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	bool wide = false;
	switch (i->id)
	{
		case RISCV_INS_SHA512SIG0:
		case RISCV_INS_SHA512SIG1:
		case RISCV_INS_SHA512SUM0:
		case RISCV_INS_SHA512SUM1:
			wide = true;
			break;
		default:
			break;
	}
	if (wide && !isXlen64())
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = wide ? irb.getInt64Ty() : irb.getInt32Ty();
	const unsigned n = wide ? 64u : 32u;
	auto* x = irb.CreateZExtOrTrunc(loadOp(ri->operands[1], irb), ty);

	auto ror = [&](unsigned k) -> llvm::Value* {
		return irb.CreateOr(
				irb.CreateLShr(x, llvm::ConstantInt::get(ty, k)),
				irb.CreateShl(x, llvm::ConstantInt::get(ty, n - k)));
	};
	auto rol = [&](unsigned k) -> llvm::Value* { return ror(n - k); };
	auto shr = [&](unsigned k) -> llvm::Value* {
		return irb.CreateLShr(x, llvm::ConstantInt::get(ty, k));
	};
	auto xor3 = [&](llvm::Value* a, llvm::Value* b, llvm::Value* c) {
		return irb.CreateXor(irb.CreateXor(a, b), c);
	};

	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case RISCV_INS_SHA256SIG0: r = xor3(ror(7), ror(18), shr(3)); break;
		case RISCV_INS_SHA256SIG1: r = xor3(ror(17), ror(19), shr(10)); break;
		case RISCV_INS_SHA256SUM0: r = xor3(ror(2), ror(13), ror(22)); break;
		case RISCV_INS_SHA256SUM1: r = xor3(ror(6), ror(11), ror(25)); break;
		case RISCV_INS_SHA512SIG0: r = xor3(ror(1), ror(8), shr(7)); break;
		case RISCV_INS_SHA512SIG1: r = xor3(ror(19), ror(61), shr(6)); break;
		case RISCV_INS_SHA512SUM0: r = xor3(ror(28), ror(34), ror(39)); break;
		case RISCV_INS_SHA512SUM1: r = xor3(ror(14), ror(18), ror(41)); break;
		case RISCV_INS_SM3P0: r = xor3(x, rol(9), rol(17)); break;
		case RISCV_INS_SM3P1: r = xor3(x, rol(15), rol(23)); break;
		default:
			throw GenericError("Unhandled insn ID in translateShaSm3().");
	}

	storeOp(ri->operands[0], irb.CreateSExtOrTrunc(r, getDefaultType()), irb);
}

/**
 * Zfa FROUND_S/D and FROUNDNX_S/D: round to an integral value in the same
 * format, under the static rounding mode the instruction names. FROUNDNX
 * additionally raises inexact, which the flags model does not carry, so
 * the two translate alike. The dynamic mode reads frm, which is not
 * tracked, and becomes the default round-to-nearest-even.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateFround(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* src = asFp(loadOp(ri->operands[1], irb), ty, irb);
	llvm::Intrinsic::ID iid = llvm::Intrinsic::nearbyint;
	switch (ri->rounding_mode)
	{
		case RISCV_RM_RTZ: iid = llvm::Intrinsic::trunc; break;
		case RISCV_RM_RDN: iid = llvm::Intrinsic::floor; break;
		case RISCV_RM_RUP: iid = llvm::Intrinsic::ceil; break;
		case RISCV_RM_RMM: iid = llvm::Intrinsic::round; break;
		case RISCV_RM_RNE: iid = llvm::Intrinsic::roundeven; break;
		default: iid = llvm::Intrinsic::nearbyint; break;
	}
	storeOp(ri->operands[0], fpIntrinsic(irb, iid, {src}), irb,
			eOpConv::FPCAST_OR_BITCAST);
}

/**
 * CBO.ZERO zeroes one cache block whose size the platform defines, so only
 * the block address goes to the pseudo call; the generic path would load
 * through it instead.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateCboZero(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1)
	{
		throwUnexpectedOperands(i);
	}
	auto* addr = amoAddress(ri->operands[0], irb);
	auto* fnc = getPseudoAsmFunction(i, irb.getVoidTy(), {addr->getType()});
	irb.CreateCall(fnc, {addr});
}

/**
 * Scalar AES and SM4 (Zkne, Zknd, Zksed) map onto LLVM's own RISC-V
 * intrinsics. The aes32 and sm4 forms work on i32 with a byte-select
 * immediate and sign-extend into rd; the aes64 forms need RV64.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateScalarCrypto(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	auto zid = [](llvm::Intrinsic::RISCVIntrinsics id) { return static_cast<llvm::Intrinsic::ID>(id); };
	llvm::Intrinsic::ID iid = llvm::Intrinsic::not_intrinsic;
	bool bs32 = false;
	switch (i->id)
	{
	case RISCV_INS_AES32DSI: iid = zid(llvm::Intrinsic::riscv_aes32dsi); bs32 = true; break;
	case RISCV_INS_AES32DSMI: iid = zid(llvm::Intrinsic::riscv_aes32dsmi); bs32 = true; break;
	case RISCV_INS_AES32ESI: iid = zid(llvm::Intrinsic::riscv_aes32esi); bs32 = true; break;
	case RISCV_INS_AES32ESMI: iid = zid(llvm::Intrinsic::riscv_aes32esmi); bs32 = true; break;
	case RISCV_INS_SM4ED: iid = zid(llvm::Intrinsic::riscv_sm4ed); bs32 = true; break;
	case RISCV_INS_SM4KS: iid = zid(llvm::Intrinsic::riscv_sm4ks); bs32 = true; break;
	case RISCV_INS_AES64DS: iid = zid(llvm::Intrinsic::riscv_aes64ds); break;
	case RISCV_INS_AES64DSM: iid = zid(llvm::Intrinsic::riscv_aes64dsm); break;
	case RISCV_INS_AES64ES: iid = zid(llvm::Intrinsic::riscv_aes64es); break;
	case RISCV_INS_AES64ESM: iid = zid(llvm::Intrinsic::riscv_aes64esm); break;
	case RISCV_INS_AES64IM: iid = zid(llvm::Intrinsic::riscv_aes64im); break;
	case RISCV_INS_AES64KS1I: iid = zid(llvm::Intrinsic::riscv_aes64ks1i); break;
	case RISCV_INS_AES64KS2: iid = zid(llvm::Intrinsic::riscv_aes64ks2); break;
	case RISCV_INS_SHA512SIG0H: iid = zid(llvm::Intrinsic::riscv_sha512sig0h); bs32 = true; break;
	case RISCV_INS_SHA512SIG0L: iid = zid(llvm::Intrinsic::riscv_sha512sig0l); bs32 = true; break;
	case RISCV_INS_SHA512SIG1H: iid = zid(llvm::Intrinsic::riscv_sha512sig1h); bs32 = true; break;
	case RISCV_INS_SHA512SIG1L: iid = zid(llvm::Intrinsic::riscv_sha512sig1l); bs32 = true; break;
	case RISCV_INS_SHA512SUM0R: iid = zid(llvm::Intrinsic::riscv_sha512sum0r); bs32 = true; break;
	case RISCV_INS_SHA512SUM1R: iid = zid(llvm::Intrinsic::riscv_sha512sum1r); bs32 = true; break;
	default: throw GenericError("Unhandled insn ID in translateScalarCrypto().");
	}
	if (!bs32 && !isXlen64())
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = bs32 ? irb.getInt32Ty() : irb.getInt64Ty();
	std::vector<llvm::Value*> args;
	for (unsigned k = 1; k < ri->op_count; ++k)
	{
		cs_riscv_op& op = ri->operands[k];
		if (op.type == RISCV_OP_IMM)
		{
			args.push_back(irb.getInt32(static_cast<uint32_t>(op.imm)));
		}
		else
		{
			args.push_back(generateTypeConversion(irb, loadOp(op, irb), ty,
					eOpConv::SEXT_TRUNC_OR_BITCAST));
		}
	}
	if (args.empty())
	{
		throwUnexpectedOperands(i);
	}
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid);
	if (f->getFunctionType()->getNumParams() != args.size())
	{
		throwUnexpectedOperands(i);
	}
	auto* r = irb.CreateCall(f, args);
	storeOp(ri->operands[0], r, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * Zcmp push/pop. Capstone 6 leaves the register list and stack adjustment
 * out of the operands, so both come from the 16-bit encoding: rlist in bits
 * 7:4 and spimm in bits 3:2. Registers are saved from the highest (s11)
 * down to ra, just under the incoming sp.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateZcmp(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (i->id == RISCV_INS_CM_MVSA01 || i->id == RISCV_INS_CM_MVA01S)
	{
		if (ri->op_count != 2)
		{
			throwUnexpectedOperands(i);
		}
		if (i->id == RISCV_INS_CM_MVSA01)
		{
			auto* a0 = loadRegister(RISCV_REG_X10, irb);
			auto* a1 = loadRegister(RISCV_REG_X11, irb);
			storeOp(ri->operands[0], a0, irb);
			storeOp(ri->operands[1], a1, irb);
		}
		else
		{
			auto* v0 = loadOp(ri->operands[0], irb);
			auto* v1 = loadOp(ri->operands[1], irb);
			storeRegister(RISCV_REG_X10, v0, irb);
			storeRegister(RISCV_REG_X11, v1, irb);
		}
		return;
	}

	if (i->size != 2)
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	unsigned enc = i->bytes[0] | (unsigned(i->bytes[1]) << 8);
	unsigned rlist = (enc >> 4) & 0xf;
	unsigned spimm = (enc >> 2) & 0x3;
	if (rlist < 4)
	{
		// Reserved; RV32E limits do not apply here.
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	static const uint32_t order[] = {
		RISCV_REG_X1, RISCV_REG_X8, RISCV_REG_X9,
		RISCV_REG_X18, RISCV_REG_X19, RISCV_REG_X20, RISCV_REG_X21,
		RISCV_REG_X22, RISCV_REG_X23, RISCV_REG_X24, RISCV_REG_X25,
		RISCV_REG_X26, RISCV_REG_X27};
	unsigned n = rlist == 15 ? 13 : rlist - 3;
	unsigned bytes = isXlen64() ? 8 : 4;
	unsigned adj = ((n * bytes + 15) & ~15u) + spimm * 16;

	auto* ty = getDefaultType();
	auto* sp = loadRegister(RISCV_REG_X2, irb);
	auto slot = [&](llvm::Value* base, int64_t off) {
		auto* a = irb.CreateAdd(base, llvm::ConstantInt::get(ty, off, true));
		return intToPtr(irb, a, ty);
	};

	if (i->id == RISCV_INS_CM_PUSH)
	{
		int64_t off = 0;
		for (unsigned k = n; k-- > 0;)
		{
			off -= bytes;
			auto* st = irb.CreateStore(loadRegister(order[k], irb), slot(sp, off));
			attachPointeeType(st, ty);
		}
		storeRegister(RISCV_REG_X2, irb.CreateSub(sp, llvm::ConstantInt::get(ty, adj)), irb);
		return;
	}

	int64_t off = adj;
	for (unsigned k = n; k-- > 0;)
	{
		off -= bytes;
		auto* ld = irb.CreateLoad(ty, slot(sp, off));
		attachPointeeType(ld, ty);
		storeRegister(order[k], ld, irb);
	}
	storeRegister(RISCV_REG_X2, irb.CreateAdd(sp, llvm::ConstantInt::get(ty, adj)), irb);
	if (i->id == RISCV_INS_CM_POPRETZ)
	{
		storeRegister(RISCV_REG_X10, llvm::ConstantInt::get(ty, 0), irb);
	}
	if (i->id == RISCV_INS_CM_POPRET || i->id == RISCV_INS_CM_POPRETZ)
	{
		generateReturnFunctionCall(irb, loadRegister(RISCV_REG_X1, irb));
	}
}

/**
 * Zcmt table jumps: the target is entry `index` of the table at jvt & ~63,
 * each entry XLEN bits wide. cm.jalt (index >= 32) also links through ra.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateZcmt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count != 1 || ri->operands[0].type != RISCV_OP_IMM)
	{
		throwUnexpectedOperands(i);
	}
	auto* ty = getDefaultType();
	unsigned bytes = isXlen64() ? 8 : 4;
	auto* jvt = loadCsr(RISCV_SYSREG_JVT, irb);
	jvt = irb.CreateAnd(irb.CreateZExtOrTrunc(jvt, ty), llvm::ConstantInt::get(ty, ~uint64_t(63)));
	auto* addr = irb.CreateAdd(jvt,
			llvm::ConstantInt::get(ty, uint64_t(ri->operands[0].imm) * bytes));
	auto* pty = llvm::PointerType::get(irb.getContext(), 0);
	auto* ld = irb.CreateLoad(ty, irb.CreateIntToPtr(addr, pty));
	attachPointeeType(ld, ty);
	llvm::Value* target = irb.CreateAnd(ld, irb.CreateNot(llvm::ConstantInt::get(ty, 1)));
	if (i->id == RISCV_INS_CM_JALT)
	{
		storeRegister(RISCV_REG_X1, getNextInsnAddress(i), irb);
		generateCallFunctionCall(irb, target);
	}
	else
	{
		generateBranchFunctionCall(irb, target);
	}
}

/**
 * Zimop mop.r.N / mop.rr.N write zero to rd when no extension redefines them.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMop(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1 || ri->operands[0].type != RISCV_OP_REG)
	{
		throwUnexpectedOperands(i);
	}
	storeOp(ri->operands[0], llvm::ConstantInt::get(getDefaultType(), 0), irb);
}

/**
 * Zfa fli.s / fli.d: Capstone hands over the constant as a double.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateFli(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count != 2 || ri->operands[0].type != RISCV_OP_REG)
	{
		throwUnexpectedOperands(i);
	}
	llvm::Value* v = nullptr;
	if (i->id == RISCV_INS_FLI_S)
	{
		v = llvm::ConstantFP::get(irb.getFloatTy(), static_cast<float>(ri->operands[1].dimm));
	}
	else
	{
		v = llvm::ConstantFP::get(irb.getDoubleTy(), ri->operands[1].dimm);
	}
	storeOp(ri->operands[0], v, irb, eOpConv::FPCAST_OR_BITCAST);
}


/**
 * Zfh half-precision scalar ops. The F registers already hold every
 * precision widened, so an H op is lifted exactly like its S twin (the
 * instruction id is swapped while the S translator runs). Half rounding of
 * the result is not modelled. FLH/FSH (in the FP load/store translators)
 * and the FMV/FCVT bit moves below are the only places a real half appears.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateZfh(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	// Capstone names the half views F*_H, which have no register globals;
	// they alias the same F registers as the F*_F single views.
	for (unsigned k = 0; k < ri->op_count; ++k)
	{
		auto& o = ri->operands[k];
		if (o.type == RISCV_OP_REG && o.reg >= RISCV_REG_F0_H && o.reg <= RISCV_REG_F31_H)
		{
			o.reg = o.reg - RISCV_REG_F0_H + RISCV_REG_F0_F;
		}
	}
	auto* half = irb.getHalfTy();
	switch (i->id)
	{
		case RISCV_INS_FMV_X_H:
		{
			auto* f = loadOp(ri->operands[1], irb);
			auto* h = irb.CreateBitCast(irb.CreateFPCast(f, half), irb.getInt16Ty());
			storeOp(ri->operands[0], irb.CreateSExt(h, getDefaultType()), irb);
			return;
		}
		case RISCV_INS_FMV_H_X:
		{
			auto* x = loadOp(ri->operands[1], irb);
			auto* h = irb.CreateBitCast(irb.CreateTrunc(x, irb.getInt16Ty()), half);
			storeOp(ri->operands[0], h, irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case RISCV_INS_FCVT_S_H:
		case RISCV_INS_FCVT_H_S:
		{
			auto* f = loadOp(ri->operands[1], irb);
			auto* t = i->id == RISCV_INS_FCVT_H_S ? half : irb.getFloatTy();
			storeOp(ri->operands[0], irb.CreateFPCast(f, t), irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case RISCV_INS_FLH:
			translateFpLoad(i, ri, irb);
			return;
		case RISCV_INS_FSH:
			translateFpStore(i, ri, irb);
			return;
		default:
			break;
	}
	unsigned sid = 0;
	void (Capstone2LlvmIrTranslatorRiscv_impl::*fn)(cs_insn*, cs_riscv*, llvm::IRBuilder<>&) = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FADD_H: sid = RISCV_INS_FADD_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpArith; break;
		case RISCV_INS_FSUB_H: sid = RISCV_INS_FSUB_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpArith; break;
		case RISCV_INS_FMUL_H: sid = RISCV_INS_FMUL_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpArith; break;
		case RISCV_INS_FDIV_H: sid = RISCV_INS_FDIV_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpArith; break;
		case RISCV_INS_FSQRT_H: sid = RISCV_INS_FSQRT_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFsqrt; break;
		case RISCV_INS_FMIN_H: sid = RISCV_INS_FMIN_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFminMax; break;
		case RISCV_INS_FMAX_H: sid = RISCV_INS_FMAX_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFminMax; break;
		case RISCV_INS_FMINM_H: sid = RISCV_INS_FMINM_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFminMax; break;
		case RISCV_INS_FMAXM_H: sid = RISCV_INS_FMAXM_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFminMax; break;
		case RISCV_INS_FMADD_H: sid = RISCV_INS_FMADD_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpFma; break;
		case RISCV_INS_FMSUB_H: sid = RISCV_INS_FMSUB_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpFma; break;
		case RISCV_INS_FNMADD_H: sid = RISCV_INS_FNMADD_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpFma; break;
		case RISCV_INS_FNMSUB_H: sid = RISCV_INS_FNMSUB_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFpFma; break;
		case RISCV_INS_FEQ_H: sid = RISCV_INS_FEQ_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp; break;
		case RISCV_INS_FLT_H: sid = RISCV_INS_FLT_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp; break;
		case RISCV_INS_FLE_H: sid = RISCV_INS_FLE_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp; break;
		case RISCV_INS_FLTQ_H: sid = RISCV_INS_FLTQ_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp; break;
		case RISCV_INS_FLEQ_H: sid = RISCV_INS_FLEQ_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp; break;
		case RISCV_INS_FSGNJ_H: sid = RISCV_INS_FSGNJ_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFsgnj; break;
		case RISCV_INS_FSGNJN_H: sid = RISCV_INS_FSGNJN_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFsgnj; break;
		case RISCV_INS_FSGNJX_H: sid = RISCV_INS_FSGNJX_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFsgnj; break;
		case RISCV_INS_FCLASS_H: sid = RISCV_INS_FCLASS_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFclass; break;
		case RISCV_INS_FROUND_H: sid = RISCV_INS_FROUND_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFround; break;
		case RISCV_INS_FROUNDNX_H: sid = RISCV_INS_FROUNDNX_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFround; break;
		case RISCV_INS_FLI_H: sid = RISCV_INS_FLI_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFli; break;
		case RISCV_INS_FCVT_W_H: sid = RISCV_INS_FCVT_W_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_WU_H: sid = RISCV_INS_FCVT_WU_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_L_H: sid = RISCV_INS_FCVT_L_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_LU_H: sid = RISCV_INS_FCVT_LU_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_H_W: sid = RISCV_INS_FCVT_S_W; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_H_WU: sid = RISCV_INS_FCVT_S_WU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_H_L: sid = RISCV_INS_FCVT_S_L; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_H_LU: sid = RISCV_INS_FCVT_S_LU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_D_H: sid = RISCV_INS_FCVT_D_S; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		case RISCV_INS_FCVT_H_D: sid = RISCV_INS_FCVT_S_D; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt; break;
		default:
			translatePseudoAsmGeneric(i, ri, irb);
			return;
	}
	unsigned hid = i->id;
	i->id = sid;
	try
	{
		(this->*fn)(i, ri, irb);
	}
	catch (...)
	{
		i->id = hid;
		throw;
	}
	i->id = hid;
}


/**
 * Instructions that behave exactly like an already-lifted twin for a
 * user-mode decompiler: the hypervisor virtual-machine loads/stores
 * HLV/HLVX/HSV are ordinary loads/stores, and the Zicfiss SSAMOSWAP is an
 * AMOSWAP on the shadow stack.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateTwin(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	unsigned sid = 0;
	void (Capstone2LlvmIrTranslatorRiscv_impl::*fn)(cs_insn*, cs_riscv*, llvm::IRBuilder<>&) = nullptr;
	switch (i->id)
	{
		case RISCV_INS_HLV_B: sid = RISCV_INS_LB; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLV_BU: sid = RISCV_INS_LBU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLV_H: sid = RISCV_INS_LH; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLV_HU: sid = RISCV_INS_LHU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLVX_HU: sid = RISCV_INS_LHU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLV_W: sid = RISCV_INS_LW; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLV_WU: sid = RISCV_INS_LWU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLVX_WU: sid = RISCV_INS_LWU; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HLV_D: sid = RISCV_INS_LD; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateLoad; break;
		case RISCV_INS_HSV_B: sid = RISCV_INS_SB; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateStore; break;
		case RISCV_INS_HSV_H: sid = RISCV_INS_SH; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateStore; break;
		case RISCV_INS_HSV_W: sid = RISCV_INS_SW; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateStore; break;
		case RISCV_INS_HSV_D: sid = RISCV_INS_SD; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateStore; break;
		case RISCV_INS_SSAMOSWAP_W: sid = RISCV_INS_AMOSWAP_W; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateAmo; break;
		case RISCV_INS_SSAMOSWAP_W_AQ: sid = RISCV_INS_AMOSWAP_W_AQ; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateAmo; break;
		case RISCV_INS_SSAMOSWAP_W_RL: sid = RISCV_INS_AMOSWAP_W_RL; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateAmo; break;
		case RISCV_INS_SSAMOSWAP_D: sid = RISCV_INS_AMOSWAP_D; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateAmo; break;
		case RISCV_INS_SSAMOSWAP_D_AQ: sid = RISCV_INS_AMOSWAP_D_AQ; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateAmo; break;
		case RISCV_INS_SSAMOSWAP_D_RL: sid = RISCV_INS_AMOSWAP_D_RL; fn = &Capstone2LlvmIrTranslatorRiscv_impl::translateAmo; break;
		default:
			translatePseudoAsmGeneric(i, ri, irb);
			return;
	}
	unsigned hid = i->id;
	i->id = sid;
	try
	{
		(this->*fn)(i, ri, irb);
	}
	catch (...)
	{
		i->id = hid;
		throw;
	}
	i->id = hid;
}


/**
 * T-Head vendor integer extensions (XTheadBa/Bb/Bs/CondMov/Mac/MemIdx
 * indexed register-offset forms), decoded from the raw little-endian word.
 * Cache-maintenance and sync forms are no-ops.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateThead(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	uint32_t w = uint32_t(i->bytes[0]) | (uint32_t(i->bytes[1]) << 8)
			| (uint32_t(i->bytes[2]) << 16) | (uint32_t(i->bytes[3]) << 24);
	unsigned rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31;
	auto* xt = getDefaultType();
	unsigned xlen = xt->getIntegerBitWidth();
	auto* i32 = irb.getInt32Ty();
	auto cx = [&](uint64_t v) { return llvm::ConstantInt::get(xt, v); };
	auto ldr = [&](unsigned n) -> llvm::Value* {
		if (n == 0) return cx(0);
		return irb.CreateZExtOrTrunc(loadRegister(RISCV_REG_X0 + n, irb), xt);
	};
	auto str = [&](unsigned n, llvm::Value* v) {
		if (n != 0) storeRegister(RISCV_REG_X0 + n, irb.CreateZExtOrTrunc(v, xt), irb);
	};
	auto sext32 = [&](llvm::Value* v) { return irb.CreateSExt(irb.CreateTrunc(v, i32), xt); };
	auto* a = ldr(rs1);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case RISCV_INS_TH_ADDSL:
			r = irb.CreateAdd(a, irb.CreateShl(ldr(rs2), (w >> 25) & 3));
			break;
		case RISCV_INS_TH_MVEQZ:
		case RISCV_INS_TH_MVNEZ:
		{
			auto* z = irb.CreateICmpEQ(ldr(rs2), cx(0));
			if (i->id == RISCV_INS_TH_MVNEZ) z = irb.CreateNot(z);
			r = irb.CreateSelect(z, a, ldr(rd));
			break;
		}
		case RISCV_INS_TH_TST:
			r = irb.CreateAnd(irb.CreateLShr(a, (w >> 20) & (xlen - 1)), cx(1));
			break;
		case RISCV_INS_TH_TSTNBZ:
		{
			r = cx(0);
			for (unsigned b = 0; b < xlen / 8; ++b)
			{
				auto* by = irb.CreateAnd(irb.CreateLShr(a, 8 * b), cx(0xff));
				auto* m = irb.CreateSelect(irb.CreateICmpEQ(by, cx(0)), cx(0xff), cx(0));
				r = irb.CreateOr(r, irb.CreateShl(m, 8 * b));
			}
			break;
		}
		case RISCV_INS_TH_REV:
		case RISCV_INS_TH_REVW:
		{
			unsigned bytes = i->id == RISCV_INS_TH_REVW ? 4 : xlen / 8;
			r = cx(0);
			for (unsigned b = 0; b < bytes; ++b)
			{
				auto* by = irb.CreateAnd(irb.CreateLShr(a, 8 * b), cx(0xff));
				r = irb.CreateOr(r, irb.CreateShl(by, 8 * (bytes - 1 - b)));
			}
			if (i->id == RISCV_INS_TH_REVW) r = sext32(r);
			break;
		}
		case RISCV_INS_TH_FF0:
		case RISCV_INS_TH_FF1:
		{
			auto* v = i->id == RISCV_INS_TH_FF0 ? irb.CreateNot(a) : a;
			auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctlz, xt);
			r = irb.CreateCall(f, {v, irb.getFalse()});
			break;
		}
		case RISCV_INS_TH_EXT:
		case RISCV_INS_TH_EXTU:
		{
			unsigned msb = (w >> 26) & 63, lsb = (w >> 20) & 63;
			if (msb >= xlen || lsb > msb) { translatePseudoAsmGeneric(i, ri, irb); return; }
			auto* t = irb.CreateShl(a, xlen - 1 - msb);
			unsigned sh = xlen - 1 - msb + lsb;
			r = i->id == RISCV_INS_TH_EXT ? irb.CreateAShr(t, sh) : irb.CreateLShr(t, sh);
			break;
		}
		case RISCV_INS_TH_SRRI:
		{
			unsigned sh = (w >> 20) & (xlen - 1);
			r = sh ? irb.CreateOr(irb.CreateLShr(a, sh), irb.CreateShl(a, xlen - sh)) : a;
			break;
		}
		case RISCV_INS_TH_SRRIW:
		{
			unsigned sh = (w >> 20) & 31;
			auto* a32 = irb.CreateTrunc(a, i32);
			llvm::Value* rr = sh ? irb.CreateOr(irb.CreateLShr(a32, sh), irb.CreateShl(a32, 32 - sh)) : a32;
			r = irb.CreateSExt(rr, xt);
			break;
		}
		case RISCV_INS_TH_MULA:
			r = irb.CreateAdd(ldr(rd), irb.CreateMul(a, ldr(rs2)));
			break;
		case RISCV_INS_TH_MULS:
			r = irb.CreateSub(ldr(rd), irb.CreateMul(a, ldr(rs2)));
			break;
		case RISCV_INS_TH_MULAW:
			r = sext32(irb.CreateAdd(ldr(rd), irb.CreateMul(a, ldr(rs2))));
			break;
		case RISCV_INS_TH_MULSW:
			r = sext32(irb.CreateSub(ldr(rd), irb.CreateMul(a, ldr(rs2))));
			break;
		case RISCV_INS_TH_MULAH:
		case RISCV_INS_TH_MULSH:
		{
			auto h = [&](llvm::Value* v) { return irb.CreateSExt(irb.CreateTrunc(v, irb.getInt16Ty()), xt); };
			auto* p = irb.CreateMul(h(a), h(ldr(rs2)));
			r = sext32(i->id == RISCV_INS_TH_MULAH ? irb.CreateAdd(ldr(rd), p) : irb.CreateSub(ldr(rd), p));
			break;
		}
		case RISCV_INS_TH_LBIA: case RISCV_INS_TH_LBIB: case RISCV_INS_TH_LBUIA: case RISCV_INS_TH_LBUIB:
		case RISCV_INS_TH_LHIA: case RISCV_INS_TH_LHIB: case RISCV_INS_TH_LHUIA: case RISCV_INS_TH_LHUIB:
		case RISCV_INS_TH_LWIA: case RISCV_INS_TH_LWIB: case RISCV_INS_TH_LWUIA: case RISCV_INS_TH_LWUIB:
		case RISCV_INS_TH_LDIA: case RISCV_INS_TH_LDIB:
		case RISCV_INS_TH_SBIA: case RISCV_INS_TH_SBIB: case RISCV_INS_TH_SHIA: case RISCV_INS_TH_SHIB:
		case RISCV_INS_TH_SWIA: case RISCV_INS_TH_SWIB: case RISCV_INS_TH_SDIA: case RISCV_INS_TH_SDIB:
		{
			// Base-update forms: rs1 += sext(imm5) << imm2, after (IA) or before (IB) the access.
			unsigned bits = 8;
			bool zx = false, st = false, after = false;
			switch (i->id)
			{
			case RISCV_INS_TH_LBIA: bits = 8; zx = false; st = false; after = true; break;
			case RISCV_INS_TH_LBIB: bits = 8; zx = false; st = false; after = false; break;
			case RISCV_INS_TH_LBUIA: bits = 8; zx = true; st = false; after = true; break;
			case RISCV_INS_TH_LBUIB: bits = 8; zx = true; st = false; after = false; break;
			case RISCV_INS_TH_LHIA: bits = 16; zx = false; st = false; after = true; break;
			case RISCV_INS_TH_LHIB: bits = 16; zx = false; st = false; after = false; break;
			case RISCV_INS_TH_LHUIA: bits = 16; zx = true; st = false; after = true; break;
			case RISCV_INS_TH_LHUIB: bits = 16; zx = true; st = false; after = false; break;
			case RISCV_INS_TH_LWIA: bits = 32; zx = false; st = false; after = true; break;
			case RISCV_INS_TH_LWIB: bits = 32; zx = false; st = false; after = false; break;
			case RISCV_INS_TH_LWUIA: bits = 32; zx = true; st = false; after = true; break;
			case RISCV_INS_TH_LWUIB: bits = 32; zx = true; st = false; after = false; break;
			case RISCV_INS_TH_LDIA: bits = 64; zx = false; st = false; after = true; break;
			case RISCV_INS_TH_LDIB: bits = 64; zx = false; st = false; after = false; break;
			case RISCV_INS_TH_SBIA: bits = 8; zx = false; st = true; after = true; break;
			case RISCV_INS_TH_SBIB: bits = 8; zx = false; st = true; after = false; break;
			case RISCV_INS_TH_SHIA: bits = 16; zx = false; st = true; after = true; break;
			case RISCV_INS_TH_SHIB: bits = 16; zx = false; st = true; after = false; break;
			case RISCV_INS_TH_SWIA: bits = 32; zx = false; st = true; after = true; break;
			case RISCV_INS_TH_SWIB: bits = 32; zx = false; st = true; after = false; break;
			case RISCV_INS_TH_SDIA: bits = 64; zx = false; st = true; after = true; break;
			case RISCV_INS_TH_SDIB: bits = 64; zx = false; st = true; after = false; break;
			default: break;
			}
			int64_t imm5 = int64_t((w >> 20) & 31);
			if (imm5 & 16) imm5 -= 32;
			auto* inc = llvm::ConstantInt::getSigned(xt, imm5 * (int64_t(1) << ((w >> 25) & 3)));
			auto* nb = irb.CreateAdd(a, inc);
			auto* ea = after ? a : nb;
			auto* ty = irb.getIntNTy(bits);
			if (st)
			{
				storeIntPtr(irb, irb.CreateTrunc(ldr(rd), ty), ea, ty);
				str(rs1, nb);
				return;
			}
			llvm::Value* l = loadIntPtr(irb, ea, ty);
			str(rs1, nb);
			str(rd, zx ? irb.CreateZExtOrTrunc(l, xt) : irb.CreateSExtOrTrunc(l, xt));
			return;
		}
		case RISCV_INS_TH_LWD: case RISCV_INS_TH_LWUD: case RISCV_INS_TH_LDD:
		case RISCV_INS_TH_SWD: case RISCV_INS_TH_SDD:
		{
			// Pair access: rd1 = rd, rd2 = rs2 at rs1 + (imm2 << (3 or 4)).
			bool dbl = i->id == RISCV_INS_TH_LDD || i->id == RISCV_INS_TH_SDD;
			bool st = i->id == RISCV_INS_TH_SWD || i->id == RISCV_INS_TH_SDD;
			unsigned bits = dbl ? 64 : 32;
			auto* ty = irb.getIntNTy(bits);
			auto* ea = irb.CreateAdd(a, cx(uint64_t((w >> 25) & 3) << (dbl ? 4 : 3)));
			auto* ea2 = irb.CreateAdd(ea, cx(bits / 8));
			if (st)
			{
				storeIntPtr(irb, irb.CreateTrunc(ldr(rd), ty), ea, ty);
				storeIntPtr(irb, irb.CreateTrunc(ldr(rs2), ty), ea2, ty);
				return;
			}
			auto* l1 = loadIntPtr(irb, ea, ty);
			auto* l2 = loadIntPtr(irb, ea2, ty);
			bool zx = i->id == RISCV_INS_TH_LWUD;
			str(rd, zx ? irb.CreateZExtOrTrunc(l1, xt) : irb.CreateSExtOrTrunc(l1, xt));
			str(rs2, zx ? irb.CreateZExtOrTrunc(l2, xt) : irb.CreateSExtOrTrunc(l2, xt));
			return;
		}
		case RISCV_INS_TH_LRB: case RISCV_INS_TH_LRBU: case RISCV_INS_TH_LRH: case RISCV_INS_TH_LRHU:
		case RISCV_INS_TH_LRW: case RISCV_INS_TH_LRWU: case RISCV_INS_TH_LRD:
		case RISCV_INS_TH_LURB: case RISCV_INS_TH_LURBU: case RISCV_INS_TH_LURH: case RISCV_INS_TH_LURHU:
		case RISCV_INS_TH_LURW: case RISCV_INS_TH_LURWU: case RISCV_INS_TH_LURD:
		case RISCV_INS_TH_SRB: case RISCV_INS_TH_SRH: case RISCV_INS_TH_SRW: case RISCV_INS_TH_SRD:
		case RISCV_INS_TH_SURB: case RISCV_INS_TH_SURH: case RISCV_INS_TH_SURW: case RISCV_INS_TH_SURD:
		{
			unsigned id = i->id;
			bool u = id == RISCV_INS_TH_LURB || id == RISCV_INS_TH_LURBU || id == RISCV_INS_TH_LURH
					|| id == RISCV_INS_TH_LURHU || id == RISCV_INS_TH_LURW || id == RISCV_INS_TH_LURWU
					|| id == RISCV_INS_TH_LURD || id == RISCV_INS_TH_SURB || id == RISCV_INS_TH_SURH
					|| id == RISCV_INS_TH_SURW || id == RISCV_INS_TH_SURD;
			llvm::Value* idx = ldr(rs2);
			if (u) idx = irb.CreateZExt(irb.CreateTrunc(idx, i32), xt);
			auto* ea = irb.CreateAdd(a, irb.CreateShl(idx, (w >> 25) & 3));
			unsigned bits = 8;
			bool zx = false, st = false;
			switch (id)
			{
				case RISCV_INS_TH_LRB: case RISCV_INS_TH_LURB: break;
				case RISCV_INS_TH_LRBU: case RISCV_INS_TH_LURBU: zx = true; break;
				case RISCV_INS_TH_LRH: case RISCV_INS_TH_LURH: bits = 16; break;
				case RISCV_INS_TH_LRHU: case RISCV_INS_TH_LURHU: bits = 16; zx = true; break;
				case RISCV_INS_TH_LRW: case RISCV_INS_TH_LURW: bits = 32; break;
				case RISCV_INS_TH_LRWU: case RISCV_INS_TH_LURWU: bits = 32; zx = true; break;
				case RISCV_INS_TH_LRD: case RISCV_INS_TH_LURD: bits = 64; break;
				case RISCV_INS_TH_SRB: case RISCV_INS_TH_SURB: st = true; break;
				case RISCV_INS_TH_SRH: case RISCV_INS_TH_SURH: bits = 16; st = true; break;
				case RISCV_INS_TH_SRW: case RISCV_INS_TH_SURW: bits = 32; st = true; break;
				default: bits = 64; st = true; break;
			}
			auto* ty = irb.getIntNTy(bits);
			if (st)
			{
				storeIntPtr(irb, irb.CreateTrunc(ldr(rd), ty), ea, ty);
				return;
			}
			llvm::Value* l = loadIntPtr(irb, ea, ty);
			r = zx ? irb.CreateZExtOrTrunc(l, xt) : irb.CreateSExtOrTrunc(l, xt);
			break;
		}
		default:
			// Cache maintenance / sync hints.
			return;
	}
	str(rd, r);
}


void Capstone2LlvmIrTranslatorRiscv_impl::translateCoreV(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	// CORE-V (XCV) ALU, bit-manipulation and packed-SIMD operations, decoded
	// from the mnemonic: cv.<op>[.sc|.sci][.b|.h].
	std::string m = i->mnemonic;
	auto* xt = getDefaultType();
	auto* i32 = irb.getInt32Ty();
	auto c32 = [&](uint64_t v) { return llvm::ConstantInt::get(i32, v); };
	auto ldo = [&](unsigned k) -> llvm::Value* {
		auto& o = ri->operands[k];
		if (o.type == RISCV_OP_IMM) return llvm::ConstantInt::getSigned(i32, o.imm);
		if (o.reg == RISCV_REG_X0) return c32(0);
		return irb.CreateZExtOrTrunc(loadRegister(o.reg, irb), i32);
	};
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ri, irb); };
	if (m.compare(0, 3, "cv.") != 0 || ri->op_count < 2 || ri->operands[0].type != RISCV_OP_REG)
	{
		fallback();
		return;
	}
	std::vector<std::string> parts;
	{
		size_t p = 3;
		while (p <= m.size())
		{
			size_t q = m.find('.', p);
			if (q == std::string::npos) q = m.size();
			parts.push_back(m.substr(p, q - p));
			p = q + 1;
		}
	}
	std::string op = parts[0];
	unsigned eb = 0;
	bool sc = false, sci = false;
	for (size_t k = 1; k < parts.size(); ++k)
	{
		if (parts[k] == "b") eb = 8;
		else if (parts[k] == "h") eb = 16;
		else if (parts[k] == "sc") sc = true;
		else if (parts[k] == "sci") sci = true;
		else { fallback(); return; }
	}
	auto* a = ldo(1);
	llvm::Value* r = nullptr;
	auto sminmax = [&](llvm::Value* x, llvm::Value* y, bool mx, bool sg) {
		auto* c = mx ? (sg ? irb.CreateICmpSGT(x, y) : irb.CreateICmpUGT(x, y))
				: (sg ? irb.CreateICmpSLT(x, y) : irb.CreateICmpULT(x, y));
		return irb.CreateSelect(c, x, y);
	};
	if (eb == 0)
	{
		auto* b = ri->op_count > 2 ? ldo(2) : nullptr;
		if (op == "abs") r = irb.CreateSelect(irb.CreateICmpSLT(a, c32(0)), irb.CreateNeg(a), a);
		else if (op == "min" && b) r = sminmax(a, b, false, true);
		else if (op == "minu" && b) r = sminmax(a, b, false, false);
		else if (op == "max" && b) r = sminmax(a, b, true, true);
		else if (op == "maxu" && b) r = sminmax(a, b, true, false);
		else if (op == "slet" && b) r = irb.CreateZExt(irb.CreateICmpSLE(a, b), i32);
		else if (op == "sletu" && b) r = irb.CreateZExt(irb.CreateICmpULE(a, b), i32);
		else if (op == "exths") r = irb.CreateSExt(irb.CreateTrunc(a, irb.getInt16Ty()), i32);
		else if (op == "exthz") r = irb.CreateZExt(irb.CreateTrunc(a, irb.getInt16Ty()), i32);
		else if (op == "extbs") r = irb.CreateSExt(irb.CreateTrunc(a, irb.getInt8Ty()), i32);
		else if (op == "extbz") r = irb.CreateZExt(irb.CreateTrunc(a, irb.getInt8Ty()), i32);
		else if (op == "cnt") r = irb.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, a);
		else if (op == "ff1") r = irb.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, a, irb.getFalse());
		else if (op == "fl1")
		{
			auto* lz = irb.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, a, irb.getFalse());
			r = irb.CreateSelect(irb.CreateICmpEQ(a, c32(0)), c32(32), irb.CreateSub(c32(31), lz));
		}
		else if (op == "clb")
		{
			auto* x = irb.CreateXor(a, irb.CreateAShr(a, 31));
			auto* lz = irb.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, x, irb.getFalse());
			r = irb.CreateSelect(irb.CreateICmpEQ(a, c32(0)), c32(0), irb.CreateSub(lz, c32(1)));
		}
		else if ((op == "clip" || op == "clipu" || op == "clipr" || op == "clipur") && b)
		{
			bool u = op == "clipu" || op == "clipur";
			llvm::Value* hi;
			llvm::Value* lo;
			if (op == "clip" || op == "clipu")
			{
				uint64_t n = uint64_t(ri->operands[2].imm) & 31;
				int64_t h = n ? (int64_t(1) << (n - 1)) - 1 : 0;
				hi = c32(h);
				lo = u ? c32(0) : llvm::ConstantInt::getSigned(i32, -h - 1);
			}
			else
			{
				hi = b;
				lo = u ? c32(0) : irb.CreateSub(irb.CreateNeg(b), c32(1));
			}
			r = irb.CreateSelect(irb.CreateICmpSGT(a, hi), hi, irb.CreateSelect(irb.CreateICmpSLT(a, lo), lo, a));
		}
		else if (op == "mac" || op == "msu")
		{
			auto* d = ldo(0);
			auto* pr = irb.CreateMul(a, b);
			r = op == "mac" ? irb.CreateAdd(d, pr) : irb.CreateSub(d, pr);
		}
		else if (std::regex_match(op, std::regex("(add|sub)(u?)(r?)n(r?)")))
		{
			// cv.add[u][r]N rd, rs1, rs2, Is3 / cv.add[u][r]Nr rd, rs1, rs2 (rd op= rs1, shift by rs2).
			std::smatch g;
			std::regex_match(op, g, std::regex("(add|sub)(u?)(r?)n(r?)"));
			bool sub = g[1] == "sub", uns = g[2] == "u", rnd = g[3] == "r", regForm = g[4] == "r";
			llvm::Value* x = regForm ? ldo(0) : a;
			llvm::Value* y = regForm ? a : b;
			llvm::Value* sh = regForm ? irb.CreateAnd(b, c32(31)) : c32(uint64_t(ri->operands[3].imm) & 31);
			if (!regForm && ri->op_count < 4) { fallback(); return; }
			llvm::Value* v = sub ? irb.CreateSub(x, y) : irb.CreateAdd(x, y);
			if (rnd)
			{
				auto* half = irb.CreateLShr(irb.CreateShl(c32(1), sh), 1);
				v = irb.CreateAdd(v, half);
			}
			r = uns ? irb.CreateLShr(v, sh) : irb.CreateAShr(v, sh);
		}
		else if (std::regex_match(op, std::regex("(mul|mac)(hh)?(s|u)(r?)n")) && ri->op_count >= 4)
		{
			std::smatch g;
			std::regex_match(op, g, std::regex("(mul|mac)(hh)?(s|u)(r?)n"));
			bool mac = g[1] == "mac", hh = g[2] == "hh", sg = g[3] == "s", rnd = g[4] == "r";
			auto half16 = [&](llvm::Value* v) {
				auto* t = irb.CreateTrunc(hh ? irb.CreateLShr(v, 16) : v, irb.getInt16Ty());
				return sg ? irb.CreateSExt(t, i32) : irb.CreateZExt(t, i32);
			};
			auto* sh = c32(uint64_t(ri->operands[3].imm) & 31);
			llvm::Value* v = irb.CreateMul(half16(a), half16(b));
			if (mac) v = irb.CreateAdd(v, ldo(0));
			if (rnd) v = irb.CreateAdd(v, irb.CreateLShr(irb.CreateShl(c32(1), sh), 1));
			r = sg ? irb.CreateAShr(v, sh) : irb.CreateLShr(v, sh);
		}
		else if (op == "bset" || op == "bclr" || op == "bsetr" || op == "bclrr" || op == "extract"
				|| op == "extractu" || op == "extractr" || op == "extractur" || op == "insert" || op == "insertr")
		{
			bool regForm = op.back() == 'r' && op != "bclr";
			std::string base = regForm ? op.substr(0, op.size() - 1) : op;
			llvm::Value* len;
			llvm::Value* pos;
			if (regForm)
			{
				if (!b) { fallback(); return; }
				len = irb.CreateAnd(irb.CreateLShr(b, 5), c32(31));
				pos = irb.CreateAnd(b, c32(31));
			}
			else
			{
				if (ri->op_count < 4) { fallback(); return; }
				len = c32(uint64_t(ri->operands[2].imm) & 31);
				pos = c32(uint64_t(ri->operands[3].imm) & 31);
			}
			// Field of len+1 bits; a full 32-bit field needs the 64-bit shift.
			auto* i64 = irb.getInt64Ty();
			auto* ones = irb.CreateTrunc(irb.CreateSub(irb.CreateShl(irb.getInt64(1),
					irb.CreateZExt(irb.CreateAdd(len, c32(1)), i64)), irb.getInt64(1)), i32);
			auto* fmask = irb.CreateShl(ones, pos);
			if (base == "bset") r = irb.CreateOr(a, fmask);
			else if (base == "bclr") r = irb.CreateAnd(a, irb.CreateNot(fmask));
			else if (base == "insert")
				r = irb.CreateOr(irb.CreateAnd(ldo(0), irb.CreateNot(fmask)), irb.CreateAnd(irb.CreateShl(a, pos), fmask));
			else
			{
				auto* f = irb.CreateAnd(irb.CreateLShr(a, pos), ones);
				if (base == "extract")
				{
					auto* up = irb.CreateSub(c32(31), len);
					f = irb.CreateAShr(irb.CreateShl(f, up), up);
				}
				r = f;
			}
		}
		else if (op == "ror" && b)
		{
			auto* s = irb.CreateAnd(b, c32(31));
			r = irb.CreateOr(irb.CreateLShr(a, s), irb.CreateShl(a, irb.CreateAnd(irb.CreateSub(c32(32), s), c32(31))));
		}
		else { fallback(); return; }
		storeRegister(ri->operands[0].reg, irb.CreateZExtOrTrunc(r, xt), irb);
		return;
	}

	// Packed SIMD.
	unsigned n = 32 / eb;
	auto* et = irb.getIntNTy(eb);
	auto lane = [&](llvm::Value* v, unsigned k) { return irb.CreateTrunc(irb.CreateLShr(v, k * eb), et); };
	llvm::Value* b = nullptr;
	if (op != "abs")
	{
		if (ri->op_count < 3) { fallback(); return; }
		b = ldo(2);
	}
	bool dot = op == "dotup" || op == "dotusp" || op == "dotsp";
	llvm::Value* acc = c32(0);
	r = c32(0);
	for (unsigned k = 0; k < n; ++k)
	{
		auto* x = lane(a, k);
		llvm::Value* y = nullptr;
		if (b) y = (sc || sci) ? irb.CreateTrunc(b, et) : lane(b, k);
		llvm::Value* e = nullptr;
		auto cmp = [&](llvm::CmpInst::Predicate p) {
			return irb.CreateSExt(irb.CreateICmp(p, x, y), et);
		};
		auto* sh = y ? irb.CreateAnd(y, llvm::ConstantInt::get(et, eb - 1)) : nullptr;
		if (op == "add") e = irb.CreateAdd(x, y);
		else if (op == "sub") e = irb.CreateSub(x, y);
		else if (op == "and") e = irb.CreateAnd(x, y);
		else if (op == "or") e = irb.CreateOr(x, y);
		else if (op == "xor") e = irb.CreateXor(x, y);
		else if (op == "avg") e = irb.CreateAShr(irb.CreateAdd(x, y), 1);
		else if (op == "avgu") e = irb.CreateLShr(irb.CreateAdd(x, y), 1);
		else if (op == "min") e = sminmax(x, y, false, true);
		else if (op == "minu") e = sminmax(x, y, false, false);
		else if (op == "max") e = sminmax(x, y, true, true);
		else if (op == "maxu") e = sminmax(x, y, true, false);
		else if (op == "sll") e = irb.CreateShl(x, sh);
		else if (op == "srl") e = irb.CreateLShr(x, sh);
		else if (op == "sra") e = irb.CreateAShr(x, sh);
		else if (op == "abs") e = irb.CreateSelect(irb.CreateICmpSLT(x, llvm::ConstantInt::get(et, 0)), irb.CreateNeg(x), x);
		else if (op == "cmpeq") e = cmp(llvm::CmpInst::ICMP_EQ);
		else if (op == "cmpne") e = cmp(llvm::CmpInst::ICMP_NE);
		else if (op == "cmpgt") e = cmp(llvm::CmpInst::ICMP_SGT);
		else if (op == "cmpge") e = cmp(llvm::CmpInst::ICMP_SGE);
		else if (op == "cmplt") e = cmp(llvm::CmpInst::ICMP_SLT);
		else if (op == "cmple") e = cmp(llvm::CmpInst::ICMP_SLE);
		else if (op == "cmpgtu") e = cmp(llvm::CmpInst::ICMP_UGT);
		else if (op == "cmpgeu") e = cmp(llvm::CmpInst::ICMP_UGE);
		else if (op == "cmpltu") e = cmp(llvm::CmpInst::ICMP_ULT);
		else if (op == "cmpleu") e = cmp(llvm::CmpInst::ICMP_ULE);
		else if (dot)
		{
			auto* xe = op == "dotsp" ? irb.CreateSExt(x, i32) : irb.CreateZExt(x, i32);
			auto* ye = op == "dotup" ? irb.CreateZExt(y, i32) : irb.CreateSExt(y, i32);
			acc = irb.CreateAdd(acc, irb.CreateMul(xe, ye));
			continue;
		}
		else { fallback(); return; }
		r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExt(e, i32), k * eb));
	}
	if (dot) r = acc;
	storeRegister(ri->operands[0].reg, irb.CreateZExtOrTrunc(r, xt), irb);
}


} // namespace capstone2llvmir
} // namespace retdec
