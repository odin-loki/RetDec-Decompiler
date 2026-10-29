/**
 * @file src/capstone2llvmir/x86/x86_sys.cpp
 * @brief x86 system, wait, shadow-stack, LWP and 4-iteration FMA translators.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/x86/x86_impl.h"

using namespace llvm;

namespace retdec {
namespace capstone2llvmir {

namespace {

bool isK(const cs_x86_op& o)
{
	return o.type == X86_OP_REG && o.reg >= X86_REG_K0 && o.reg <= X86_REG_K7;
}

} // anonymous namespace

/**
 * Instructions whose effect lives outside the modelled state (MSRs, XCRs,
 * the shadow stack pointer, monitors, LWP) become pseudo calls, but every
 * architectural register and flag result they define is written explicitly.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSystemX86(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	auto* i1 = irb.getInt1Ty();
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto clearArith = [&](Value* cf) {
		storeRegister(X86_REG_CF, cf, irb);
		for (auto r : {X86_REG_ZF, X86_REG_SF, X86_REG_OF, X86_REG_AF, X86_REG_PF})
			storeRegister(r, irb.getFalse(), irb);
	};
	auto edxEax = [&]() {
		Value* lo = irb.CreateZExtOrTrunc(loadRegister(X86_REG_EAX, irb), i64);
		Value* hi = irb.CreateZExtOrTrunc(loadRegister(X86_REG_EDX, irb), i64);
		return irb.CreateOr(lo, irb.CreateShl(hi, 32));
	};
	auto setEdxEax = [&](Value* v) {
		storeRegister(X86_REG_EAX, irb.CreateTrunc(v, i32), irb);
		storeRegister(X86_REG_EDX, irb.CreateTrunc(irb.CreateLShr(v, 32), i32), irb);
	};
	switch (i->id)
	{
		case X86_INS_UMWAIT:
		case X86_INS_TPAUSE:
		{
			// The operand is an implicit-deadline hint; EDX:EAX is the TSC deadline.
			Value* hint = irb.CreateZExtOrTrunc(loadOp(xi->operands[0], irb), i32);
			auto* fn = getPseudoAsmFunction(i, i1, ArrayRef<Type*>{i32, i64});
			clearArith(irb.CreateCall(fn, {hint, edxEax()}));
			return;
		}
		case X86_INS_XTEST:
		{
			auto* fn = getPseudoAsmFunction(i, i1, ArrayRef<Type*>{});
			Value* inTx = irb.CreateCall(fn);
			clearArith(irb.getFalse());
			storeRegister(X86_REG_ZF, irb.CreateNot(inTx), irb);
			return;
		}
		case X86_INS_XGETBV:
		case X86_INS_RDMSR:
		case X86_INS_RDPMC:
		{
			Value* ecx = irb.CreateZExtOrTrunc(loadRegister(X86_REG_ECX, irb), i32);
			auto* fn = getPseudoAsmFunction(i, i64, ArrayRef<Type*>{i32});
			setEdxEax(irb.CreateCall(fn, {ecx}));
			return;
		}
		case X86_INS_XSETBV:
		case X86_INS_WRMSR:
		{
			Value* ecx = irb.CreateZExtOrTrunc(loadRegister(X86_REG_ECX, irb), i32);
			auto* fn = getPseudoAsmFunction(i, irb.getVoidTy(), ArrayRef<Type*>{i32, i64});
			irb.CreateCall(fn, {ecx, edxEax()});
			return;
		}
		case X86_INS_LWPINS:
		{
			std::vector<Value*> a;
			std::vector<Type*> t;
			for (unsigned k = 0; k < xi->op_count; ++k)
			{
				a.push_back(loadOp(xi->operands[k], irb));
				t.push_back(a.back()->getType());
			}
			auto* fn = getPseudoAsmFunction(i, i1, t);
			storeRegister(X86_REG_CF, irb.CreateCall(fn, a), irb);
			return;
		}
		case X86_INS_WRSSD:
		case X86_INS_WRSSQ:
		case X86_INS_WRUSSD:
		case X86_INS_WRUSSQ:
		{
			// A store to (shadow-stack) memory.
			EXPECT_IS_BINARY(i, xi, irb);
			storeOp(xi->operands[0], loadOp(xi->operands[1], irb), irb);
			return;
		}
		case X86_INS_VGATHERPF0DPD: case X86_INS_VGATHERPF0DPS: case X86_INS_VGATHERPF0QPD: case X86_INS_VGATHERPF0QPS:
		case X86_INS_VGATHERPF1DPD: case X86_INS_VGATHERPF1DPS: case X86_INS_VGATHERPF1QPD: case X86_INS_VGATHERPF1QPS:
		case X86_INS_VSCATTERPF0DPD: case X86_INS_VSCATTERPF0DPS: case X86_INS_VSCATTERPF0QPD: case X86_INS_VSCATTERPF0QPS:
		case X86_INS_VSCATTERPF1DPD: case X86_INS_VSCATTERPF1DPS: case X86_INS_VSCATTERPF1QPD: case X86_INS_VSCATTERPF1QPS:
			// Prefetch hints: no architectural effect.
			return;
		case X86_INS_SWAPGS:
		case X86_INS_LOCK:
		case X86_INS_REP:
		case X86_INS_REPNE:
		case X86_INS_DATA16:
		case X86_INS_REX64:
		case X86_INS_XACQUIRE:
		case X86_INS_XRELEASE:
		default:
			translatePseudoAsmGeneric(i, xi, irb);
	}
}

/**
 * AVX512_4FMAPS / AVX512_4VNNIW: four back-to-back multiply-accumulates
 * whose register source is a block of four consecutive registers and whose
 * memory source is four 32-bit scalars.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateFourIter(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	int k = -1;
	std::vector<unsigned> ops;
	for (unsigned j = 0; j < xi->op_count; ++j)
	{
		if (isK(xi->operands[j])) k = int(j);
		else ops.push_back(j);
	}
	if (ops.size() != 3 || xi->operands[ops[0]].type != X86_OP_REG
			|| xi->operands[ops[1]].type != X86_OP_REG
			|| xi->operands[ops[2]].type != X86_OP_MEM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	bool scalar = i->id == X86_INS_V4FMADDSS || i->id == X86_INS_V4FNMADDSS;
	bool neg = i->id == X86_INS_V4FNMADDPS || i->id == X86_INS_V4FNMADDSS;
	bool fp = i->id != X86_INS_VP4DPWSSD && i->id != X86_INS_VP4DPWSSDS;
	bool sat = i->id == X86_INS_VP4DPWSSDS;
	auto& dop = xi->operands[ops[0]];
	unsigned bits = 0;
	Value* d = loadVectorOp(dop, irb, bits);
	unsigned mbits = 128;
	Value* m = loadVectorOp(xi->operands[ops[2]], irb, mbits);
	if (!d || !m)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = bits / 32;
	auto* i32 = irb.getInt32Ty();
	auto* f32 = irb.getFloatTy();
	auto* vi = FixedVectorType::get(i32, n);
	Value* acc = irb.CreateBitCast(d, vi);
	Value* mv = irb.CreateBitCast(m, FixedVectorType::get(i32, 4));
	unsigned base = xi->operands[ops[1]].reg;
	// The block is the source register rounded down to a multiple of four.
	unsigned idx = 0;
	unsigned bank = 0;
	if (base >= X86_REG_ZMM0 && base <= X86_REG_ZMM31) { bank = X86_REG_ZMM0; idx = base - X86_REG_ZMM0; }
	else if (base >= X86_REG_XMM0 && base <= X86_REG_XMM31) { bank = X86_REG_XMM0; idx = base - X86_REG_XMM0; }
	else
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	idx &= ~3u;
	Value* fAcc = irb.CreateBitCast(acc, FixedVectorType::get(f32, n));
	for (unsigned it = 0; it < 4; ++it)
	{
		cs_x86_op so = xi->operands[ops[1]];
		so.reg = x86_reg(bank + idx + it);
		unsigned sb = 0;
		Value* s = loadVectorOp(so, irb, sb);
		if (sb != bits)
		{
			s = irb.CreateZExtOrTrunc(s, irb.getIntNTy(bits));
		}
		Value* mi = irb.CreateExtractElement(mv, irb.getInt32(it));
		if (fp)
		{
			Value* sv = irb.CreateBitCast(s, FixedVectorType::get(f32, n));
			Value* mf = irb.CreateVectorSplat(n, irb.CreateBitCast(mi, f32));
			Value* p = irb.CreateFMul(sv, mf);
			Value* r = neg ? irb.CreateFSub(fAcc, p) : irb.CreateFAdd(fAcc, p);
			if (scalar)
			{
				r = irb.CreateInsertElement(fAcc, irb.CreateExtractElement(r, uint64_t(0)), uint64_t(0));
			}
			fAcc = r;
		}
		else
		{
			auto* i16 = irb.getInt16Ty();
			auto* vw = FixedVectorType::get(i16, 2 * n);
			Value* sw = irb.CreateSExt(irb.CreateBitCast(s, vw), FixedVectorType::get(i32, 2 * n));
			Value* mw = irb.CreateBitCast(mi, FixedVectorType::get(i16, 2));
			std::vector<int> ev, od;
			for (unsigned j = 0; j < n; ++j) { ev.push_back(2 * j); od.push_back(2 * j + 1); }
			Value* m0 = irb.CreateVectorSplat(n, irb.CreateSExt(irb.CreateExtractElement(mw, uint64_t(0)), i32));
			Value* m1 = irb.CreateVectorSplat(n, irb.CreateSExt(irb.CreateExtractElement(mw, uint64_t(1)), i32));
			Value* p0 = irb.CreateMul(irb.CreateShuffleVector(sw, ev), m0);
			Value* p1 = irb.CreateMul(irb.CreateShuffleVector(sw, od), m1);
			if (sat)
			{
				// Each product fits in 32 bits; only the sums can overflow.
				auto* v64 = FixedVectorType::get(irb.getInt64Ty(), n);
				Value* s64 = irb.CreateAdd(irb.CreateAdd(irb.CreateSExt(acc, v64), irb.CreateSExt(p0, v64)), irb.CreateSExt(p1, v64));
				Value* hi = ConstantInt::get(v64, INT32_MAX);
				Value* lo = ConstantInt::get(v64, uint64_t(int64_t(INT32_MIN)));
				s64 = irb.CreateSelect(irb.CreateICmpSGT(s64, hi), hi, s64);
				s64 = irb.CreateSelect(irb.CreateICmpSLT(s64, lo), lo, s64);
				acc = irb.CreateTrunc(s64, vi);
			}
			else
			{
				acc = irb.CreateAdd(acc, irb.CreateAdd(p0, p1));
			}
		}
	}
	Value* res = fp ? irb.CreateBitCast(fAcc, vi) : acc;
	if (k >= 0)
	{
		bool zero = std::string(i->op_str).find("{z}") != std::string::npos;
		Value* mk = irb.CreateZExtOrTrunc(loadRegister(xi->operands[k].reg, irb), irb.getIntNTy(n));
		Value* lanes = irb.CreateBitCast(mk, FixedVectorType::get(irb.getInt1Ty(), n));
		if (scalar)
		{
			lanes = irb.CreateInsertElement(Constant::getAllOnesValue(lanes->getType()),
				irb.CreateExtractElement(lanes, uint64_t(0)), uint64_t(0));
		}
		Value* old = irb.CreateBitCast(d, vi);
		if (zero && scalar)
		{
			// Zeroing only clears lane 0; the upper lanes are preserved.
			old = irb.CreateInsertElement(old, irb.getInt32(0), uint64_t(0));
		}
		else if (zero)
		{
			old = Constant::getNullValue(vi);
		}
		res = irb.CreateSelect(lanes, res, old);
	}
	storeVectorOp(dop, irb.CreateBitCast(res, irb.getIntNTy(bits)), bits, irb);
}


/**
 * SSE4a EXTRQ/INSERTQ, MMX MASKMOVQ, XOP VPMACS[S]WD/VPMADCS[S]WD and
 * VPERMIL2PS/PD.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateX86Misc2(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	auto* i8 = irb.getInt8Ty();
	auto* i16 = irb.getInt16Ty();
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getInt128Ty();
	auto vec = [&](unsigned k, unsigned want) -> Value* {
		unsigned b = want;
		Value* v = loadVectorOp(xi->operands[k], irb, b);
		return v ? irb.CreateZExtOrTrunc(v, irb.getIntNTy(want)) : nullptr;
	};
	auto lowMask = [&](Value* len) {
		// len == 0 means 64.
		Value* m = irb.CreateSub(irb.CreateShl(ConstantInt::get(i64, 1), len), ConstantInt::get(i64, 1));
		return irb.CreateSelect(irb.CreateICmpEQ(len, ConstantInt::get(i64, 0)), ConstantInt::get(i64, ~0ULL), m);
	};
	switch (i->id)
	{
		case X86_INS_EXTRQ:
		case X86_INS_INSERTQ:
		{
			bool ins = i->id == X86_INS_INSERTQ;
			Value* d = vec(0, 128);
			if (!d) break;
			Value* len;
			Value* idx;
			Value* src = nullptr;
			unsigned nImm = 0;
			for (unsigned k = 0; k < xi->op_count; ++k)
				nImm += xi->operands[k].type == X86_OP_IMM;
			if (nImm == 2)
			{
				unsigned b = ins ? 2 : 1;
				len = ConstantInt::get(i64, xi->operands[b].imm & 63);
				idx = ConstantInt::get(i64, xi->operands[b + 1].imm & 63);
				if (ins) src = irb.CreateTrunc(vec(1, 128), i64);
			}
			else
			{
				Value* c = vec(1, 128);
				if (!c) break;
				if (ins)
				{
					src = irb.CreateTrunc(c, i64);
					Value* ctl = irb.CreateTrunc(irb.CreateLShr(c, 64), i64);
					len = irb.CreateAnd(ctl, 63);
					idx = irb.CreateAnd(irb.CreateLShr(ctl, 8), 63);
				}
				else
				{
					Value* ctl = irb.CreateTrunc(c, i64);
					len = irb.CreateAnd(ctl, 63);
					idx = irb.CreateAnd(irb.CreateLShr(ctl, 8), 63);
				}
			}
			Value* lo = irb.CreateTrunc(d, i64);
			Value* m = lowMask(len);
			Value* r;
			if (ins)
			{
				Value* fm = irb.CreateShl(m, idx);
				r = irb.CreateOr(irb.CreateAnd(lo, irb.CreateNot(fm)),
					irb.CreateAnd(irb.CreateShl(src, idx), fm));
			}
			else
			{
				r = irb.CreateAnd(irb.CreateLShr(lo, idx), m);
			}
			Value* hiPart = irb.CreateAnd(d, ConstantInt::get(i128, APInt::getHighBitsSet(128, 64)));
			storeVectorOp(xi->operands[0], irb.CreateOr(hiPart, irb.CreateZExt(r, i128)), 128, irb);
			return;
		}
		case X86_INS_MASKMOVQ:
		{
			if (xi->op_count != 2) break;
			auto* v8 = FixedVectorType::get(i8, 8);
			Value* data = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadOp(xi->operands[0], irb), i64), v8);
			Value* mask = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadOp(xi->operands[1], irb), i64), v8);
			Value* on = irb.CreateICmpSLT(mask, Constant::getNullValue(v8));
			bool is64 = _origBasicMode == CS_MODE_64;
			Value* a = loadRegister(is64 ? X86_REG_RDI : X86_REG_EDI, irb);
			Value* ptr = irb.CreateIntToPtr(a, PointerType::get(irb.getContext(), 0));
			irb.CreateMaskedStore(data, ptr, Align(1), on);
			return;
		}
		case X86_INS_VPMACSWD:
		case X86_INS_VPMACSSWD:
		case X86_INS_VPMADCSWD:
		case X86_INS_VPMADCSSWD:
		{
			if (xi->op_count != 4) break;
			Value* a = vec(1, 128);
			Value* b = vec(2, 128);
			Value* c = vec(3, 128);
			if (!a || !b || !c) break;
			auto* v8w = FixedVectorType::get(i16, 8);
			auto* v4 = FixedVectorType::get(i32, 4);
			auto* v4l = FixedVectorType::get(i64, 4);
			Value* aw = irb.CreateSExt(irb.CreateBitCast(a, v8w), FixedVectorType::get(i64, 8));
			Value* bw = irb.CreateSExt(irb.CreateBitCast(b, v8w), FixedVectorType::get(i64, 8));
			Value* p = irb.CreateMul(aw, bw);
			Value* odd = irb.CreateShuffleVector(p, std::vector<int>{1, 3, 5, 7});
			Value* sum = odd;
			if (i->id == X86_INS_VPMADCSWD || i->id == X86_INS_VPMADCSSWD)
				sum = irb.CreateAdd(sum, irb.CreateShuffleVector(p, std::vector<int>{0, 2, 4, 6}));
			sum = irb.CreateAdd(sum, irb.CreateSExt(irb.CreateBitCast(c, v4), v4l));
			if (i->id == X86_INS_VPMACSSWD || i->id == X86_INS_VPMADCSSWD)
			{
				Value* hi = ConstantInt::get(v4l, INT32_MAX);
				Value* lo = ConstantInt::get(v4l, uint64_t(int64_t(INT32_MIN)));
				sum = irb.CreateSelect(irb.CreateICmpSGT(sum, hi), hi, sum);
				sum = irb.CreateSelect(irb.CreateICmpSLT(sum, lo), lo, sum);
			}
			storeVectorOp(xi->operands[0], irb.CreateBitCast(irb.CreateTrunc(sum, v4), i128), 128, irb);
			return;
		}
		case X86_INS_VPERMIL2PS:
		case X86_INS_VPERMIL2PD:
		{
			if (xi->op_count != 5 || xi->operands[4].type != X86_OP_IMM) break;
			bool ps = i->id == X86_INS_VPERMIL2PS;
			unsigned bits = xi->operands[0].size * 8;
			if (bits != 128 && bits != 256) break;
			Value* s1 = vec(1, bits);
			Value* s2 = vec(2, bits);
			Value* sel = vec(3, bits);
			if (!s1 || !s2 || !sel) break;
			unsigned eb = ps ? 32 : 64;
			unsigned n = bits / eb;
			unsigned per = 128 / eb;
			auto* et = irb.getIntNTy(eb);
			auto* vt = FixedVectorType::get(et, n);
			Value* a = irb.CreateBitCast(s1, vt);
			Value* b = irb.CreateBitCast(s2, vt);
			Value* sv = irb.CreateBitCast(sel, vt);
			unsigned m2z = xi->operands[4].imm & 3;
			Value* r = UndefValue::get(vt);
			for (unsigned j = 0; j < n; ++j)
			{
				unsigned laneBase = (j / per) * per;
				Value* s = irb.CreateExtractElement(sv, uint64_t(j));
				Value* eidx = ps ? irb.CreateAnd(s, 3) : irb.CreateAnd(irb.CreateLShr(s, 1), 1);
				Value* fromB = irb.CreateICmpNE(irb.CreateAnd(s, 4), ConstantInt::get(et, 0));
				Value* pos = irb.CreateAdd(irb.CreateZExtOrTrunc(eidx, i32), irb.getInt32(laneBase));
				Value* e = irb.CreateSelect(fromB, irb.CreateExtractElement(b, pos), irb.CreateExtractElement(a, pos));
				if (m2z >= 2)
				{
					Value* match = irb.CreateICmpNE(irb.CreateAnd(s, 8), ConstantInt::get(et, 0));
					Value* zero = m2z == 2 ? match : irb.CreateNot(match);
					e = irb.CreateSelect(zero, ConstantInt::get(et, 0), e);
				}
				r = irb.CreateInsertElement(r, e, uint64_t(j));
			}
			storeVectorOp(xi->operands[0], irb.CreateBitCast(r, irb.getIntNTy(bits)), bits, irb);
			return;
		}
		default:
			break;
	}
	translatePseudoAsmGeneric(i, xi, irb);
}

/**
 * AVX512ER: VRCP28, VRSQRT28 and VEXP2 approximate to 2^-28, which lifts as
 * the exact operation.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateEr28(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	int k = -1;
	std::vector<unsigned> ops;
	for (unsigned j = 0; j < xi->op_count; ++j)
	{
		if (isK(xi->operands[j])) k = int(j);
		else ops.push_back(j);
	}
	bool dbl = false, scalar = false;
	int kind = 0; // 0 rcp, 1 rsqrt, 2 exp2
	switch (i->id)
	{
		case X86_INS_VRCP28PD: dbl = true; break;
		case X86_INS_VRCP28PS: break;
		case X86_INS_VRCP28SD: dbl = scalar = true; break;
		case X86_INS_VRCP28SS: scalar = true; break;
		case X86_INS_VRSQRT28PD: dbl = true; kind = 1; break;
		case X86_INS_VRSQRT28PS: kind = 1; break;
		case X86_INS_VRSQRT28SD: dbl = scalar = true; kind = 1; break;
		case X86_INS_VRSQRT28SS: scalar = true; kind = 1; break;
		case X86_INS_VEXP2PD: dbl = true; kind = 2; break;
		default: kind = 2; break;
	}
	if (ops.size() != (scalar ? 3u : 2u))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto& dop = xi->operands[ops[0]];
	unsigned bits = scalar ? 128 : dop.size * 8;
	Type* ft = dbl ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned eb = dbl ? 64 : 32;
	unsigned n = bits / eb;
	auto* vf = FixedVectorType::get(ft, n);
	auto* vi = FixedVectorType::get(irb.getIntNTy(eb), n);
	unsigned db = 0;
	Value* d = loadVectorOp(dop, irb, db);
	if (!d)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	d = irb.CreateZExtOrTrunc(d, irb.getIntNTy(bits));
	auto f = [&](Value* x) -> Value* {
		if (kind == 2) return irb.CreateUnaryIntrinsic(Intrinsic::exp2, x);
		if (kind == 1) x = irb.CreateUnaryIntrinsic(Intrinsic::sqrt, x);
		return irb.CreateFDiv(ConstantFP::get(x->getType(), 1.0), x);
	};
	Value* res;
	Value* base;
	if (scalar)
	{
		unsigned b1 = 128;
		Value* s1 = loadVectorOp(xi->operands[ops[1]], irb, b1);
		auto& so = xi->operands[ops[2]];
		Value* x;
		if (so.type == X86_OP_MEM)
			x = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadOp(so, irb), irb.getIntNTy(eb)), ft);
		else
		{
			unsigned b2 = 128;
			Value* s2 = loadVectorOp(so, irb, b2);
			x = irb.CreateExtractElement(irb.CreateBitCast(irb.CreateZExtOrTrunc(s2, irb.getInt128Ty()), vf), uint64_t(0));
		}
		base = irb.CreateBitCast(irb.CreateZExtOrTrunc(s1, irb.getInt128Ty()), vf);
		res = irb.CreateInsertElement(base, f(x), uint64_t(0));
	}
	else
	{
		unsigned sb = bits;
		Value* s = loadVectorOp(xi->operands[ops[1]], irb, sb);
		if (!s)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		base = irb.CreateBitCast(d, vf);
		res = f(irb.CreateBitCast(irb.CreateZExtOrTrunc(s, irb.getIntNTy(bits)), vf));
	}
	Value* ri = irb.CreateBitCast(res, vi);
	if (k >= 0)
	{
		bool zero = std::string(i->op_str).find("{z}") != std::string::npos;
		Value* mk = irb.CreateZExtOrTrunc(loadRegister(xi->operands[k].reg, irb), irb.getIntNTy(n));
		Value* lanes = irb.CreateBitCast(mk, FixedVectorType::get(irb.getInt1Ty(), n));
		Value* old = scalar ? irb.CreateBitCast(base, vi) : irb.CreateBitCast(d, vi);
		if (scalar)
		{
			lanes = irb.CreateInsertElement(Constant::getAllOnesValue(lanes->getType()),
				irb.CreateExtractElement(lanes, uint64_t(0)), uint64_t(0));
			if (zero) old = irb.CreateInsertElement(old, ConstantInt::get(irb.getIntNTy(eb), 0), uint64_t(0));
		}
		else if (zero)
		{
			old = Constant::getNullValue(vi);
		}
		ri = irb.CreateSelect(lanes, ri, old);
	}
	storeVectorOp(dop, irb.CreateBitCast(ri, irb.getIntNTy(bits)), bits, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
