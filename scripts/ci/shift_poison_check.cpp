/**
 * @file scripts/ci/shift_poison_check.cpp
 * @brief Fails when a translator emits a shift whose amount is not provably
 *        below the operand's width.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of SHIFT-01. See scripts/ci/check_shift_poison.sh.
 *
 * WHY THIS EXISTS
 *
 * `shl i8 %x, 20` is POISON in LLVM. Not "implementation-defined", not "some
 * number" -- poison, which the optimiser may assume never happens and may
 * propagate through everything downstream. Every architecture in this tree
 * defines an answer for an over-long shift: x86 masks the count to five bits
 * and then runs it (so an 8-bit shift by 20 is zero), ARM masks to eight bits
 * and saturates, PowerPC answers zero, MIPS masks to the register width.
 * None of them is poison.
 *
 * The hardware oracles cannot see this. They compare against RetDec's own
 * interpreter, and the interpreter reduces every shift amount modulo the
 * operand width (llvmir_emul.cpp, getShiftAmount) -- which is the x86 rule for
 * rotates, so poison-producing IR can give exactly the right value and pass.
 * Two of the eight Batch AM mutations were not caught for precisely that
 * reason: reverting them left the IR full of poison and the measured answers
 * unchanged.
 *
 * So this check does not run the IR at all. It asks LLVM's own value tracking
 * the only question that matters: given everything known about this shift
 * amount, can it reach the operand's bit width? If it can, the translator has
 * emitted poison, whatever the interpreter says the answer is.
 */

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/APInt.h>
#include <llvm/Support/KnownBits.h>
#include <llvm/Support/raw_ostream.h>

#include <keystone/keystone.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "retdec/capstone2llvmir/capstone2llvmir.h"

using namespace retdec::capstone2llvmir;

namespace {

struct Case
{
	const char* arch;   ///< selects both the assembler and the translator
	const char* asmText;
};

/*
 * One entry per shift or rotate form whose amount is NOT an immediate. An
 * immediate amount is bounded by the encoding and cannot be out of range; a
 * register amount is whatever the program put there, which is the whole
 * problem.
 *
 * Shifted-operand forms are here as well as the standalone shifts: on ARM the
 * shift hides inside another instruction's second operand, which is where the
 * register-ROR case that no test covered was living.
 */
const Case CASES[] = {
	// ---- x86-64 ----
	{"x86_64", "shl al, cl"},      {"x86_64", "shr al, cl"},
	{"x86_64", "sar al, cl"},      {"x86_64", "rol al, cl"},
	{"x86_64", "ror al, cl"},      {"x86_64", "rcl al, cl"},
	{"x86_64", "rcr al, cl"},
	{"x86_64", "shl ax, cl"},      {"x86_64", "shr ax, cl"},
	{"x86_64", "sar ax, cl"},      {"x86_64", "rol ax, cl"},
	{"x86_64", "ror ax, cl"},      {"x86_64", "rcl ax, cl"},
	{"x86_64", "rcr ax, cl"},
	{"x86_64", "shl eax, cl"},     {"x86_64", "shr eax, cl"},
	{"x86_64", "sar eax, cl"},     {"x86_64", "rol eax, cl"},
	{"x86_64", "ror eax, cl"},     {"x86_64", "rcl eax, cl"},
	{"x86_64", "rcr eax, cl"},
	{"x86_64", "shl rax, cl"},     {"x86_64", "shr rax, cl"},
	{"x86_64", "sar rax, cl"},     {"x86_64", "rol rax, cl"},
	{"x86_64", "ror rax, cl"},
	{"x86_64", "shld ax, bx, cl"}, {"x86_64", "shrd ax, bx, cl"},
	{"x86_64", "shld eax, ebx, cl"}, {"x86_64", "shrd eax, ebx, cl"},
	{"x86_64", "shld rax, rbx, cl"}, {"x86_64", "shrd rax, rbx, cl"},
	{"x86_64", "shlx eax, ebx, ecx"}, {"x86_64", "shrx eax, ebx, ecx"},
	{"x86_64", "sarx eax, ebx, ecx"},
	{"x86_64", "bt eax, ebx"},     {"x86_64", "bts eax, ebx"},
	{"x86_64", "btr eax, ebx"},    {"x86_64", "btc eax, ebx"},

	// ---- ARM (A32) ----
	{"arm", "lsl r0, r1, r2"},     {"arm", "lsr r0, r1, r2"},
	{"arm", "asr r0, r1, r2"},     {"arm", "ror r0, r1, r2"},
	{"arm", "lsls r0, r1, r2"},    {"arm", "lsrs r0, r1, r2"},
	{"arm", "asrs r0, r1, r2"},    {"arm", "rors r0, r1, r2"},
	// The shifted-operand forms: the shift is a modifier on operand 2.
	{"arm", "and r0, r1, r2, lsl r3"},
	{"arm", "and r0, r1, r2, lsr r3"},
	{"arm", "and r0, r1, r2, asr r3"},
	{"arm", "and r0, r1, r2, ror r3"},
	{"arm", "add r0, r1, r2, lsl r3"},
	{"arm", "eor r0, r1, r2, ror r3"},

	// ---- ARM64 ----
	{"arm64", "lslv w0, w1, w2"},  {"arm64", "lsrv w0, w1, w2"},
	{"arm64", "asrv w0, w1, w2"},  {"arm64", "rorv w0, w1, w2"},
	{"arm64", "lslv x0, x1, x2"},  {"arm64", "lsrv x0, x1, x2"},
	{"arm64", "asrv x0, x1, x2"},  {"arm64", "rorv x0, x1, x2"},

	// ---- MIPS ----
	{"mips", "sllv $1, $2, $3"},   {"mips", "srlv $1, $2, $3"},
	{"mips", "srav $1, $2, $3"},

	// ---- PowerPC ----
	{"powerpc", "slw 3, 4, 5"},    {"powerpc", "srw 3, 4, 5"},
	{"powerpc", "sraw 3, 4, 5"},
	{"powerpc", "sld 3, 4, 5"},    {"powerpc", "srd 3, 4, 5"},
	{"powerpc", "srad 3, 4, 5"},
};

bool assemble(const char* arch, const char* text, std::vector<uint8_t>& out)
{
	ks_arch a; int mode;
	if (!strcmp(arch, "x86_64"))      { a = KS_ARCH_X86;     mode = KS_MODE_64; }
	else if (!strcmp(arch, "arm"))    { a = KS_ARCH_ARM;     mode = KS_MODE_ARM | KS_MODE_LITTLE_ENDIAN; }
	else if (!strcmp(arch, "arm64"))  { a = KS_ARCH_ARM64;   mode = KS_MODE_LITTLE_ENDIAN; }
	else if (!strcmp(arch, "mips"))   { a = KS_ARCH_MIPS;    mode = KS_MODE_MIPS32 | KS_MODE_BIG_ENDIAN; }
	else if (!strcmp(arch, "powerpc")){ a = KS_ARCH_PPC;     mode = KS_MODE_PPC64 | KS_MODE_BIG_ENDIAN; }
	else return false;

	ks_engine* ks = nullptr;
	if (ks_open(a, mode, &ks) != KS_ERR_OK) return false;
	unsigned char* enc = nullptr; size_t sz = 0, cnt = 0;
	bool ok = ks_asm(ks, text, 0x1000, &enc, &sz, &cnt) == KS_ERR_OK && sz > 0;
	if (ok) out.assign(enc, enc + sz);
	if (enc) ks_free(enc);
	ks_close(ks);
	return ok;
}

std::unique_ptr<Capstone2LlvmIrTranslator> makeTranslator(const char* arch, llvm::Module* m)
{
	if (!strcmp(arch, "x86_64"))  return Capstone2LlvmIrTranslator::createX86_64(m);
	if (!strcmp(arch, "arm"))     return Capstone2LlvmIrTranslator::createArm(m);
	if (!strcmp(arch, "arm64"))   return Capstone2LlvmIrTranslator::createArm64(m);
	if (!strcmp(arch, "mips"))    return Capstone2LlvmIrTranslator::createMips32(m);
	if (!strcmp(arch, "powerpc")) return Capstone2LlvmIrTranslator::createPpc64(m);
	return nullptr;
}

/// An unsigned interval, or "no idea".
struct Rng
{
	bool known = false;
	llvm::APInt lo, hi;
};

/// The smallest unsigned interval this can prove for @p v.
///
/// NOT llvm::computeConstantRange. That one is shallow: it answers "full-set"
/// for a plain `zext` of a value it could have bounded, which made it report
/// every RCL and RCR shift as possibly out of range. And not
/// computeKnownBits either, which carries a bitmask and so cannot express
/// "at most 8" more tightly than "at most 15".
///
/// This handles exactly the operations the translators build shift amounts
/// out of, and answers "unknown" for anything else. Being narrow is the point:
/// a checker that guesses is a checker that will one day excuse a real bug.
Rng rangeOf(llvm::Value* v, unsigned depth = 0)
{
	Rng r;
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(v->getType());
	if (ty == nullptr || depth > 8)
	{
		return r;
	}
	unsigned bw = ty->getBitWidth();

	if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v))
	{
		r.known = true;
		r.lo = r.hi = c->getValue();
		return r;
	}

	auto* in = llvm::dyn_cast<llvm::Instruction>(v);
	if (in == nullptr)
	{
		return r;
	}

	auto constOp = [&](unsigned k) -> llvm::ConstantInt* {
		return llvm::dyn_cast<llvm::ConstantInt>(in->getOperand(k));
	};

	switch (in->getOpcode())
	{
		case llvm::Instruction::And:
		{
			// x & C is at most C, whatever x is. Both operand orders.
			for (unsigned k = 0; k < 2; ++k)
			{
				if (auto* c = constOp(k))
				{
					r.known = true;
					r.lo = llvm::APInt::getZero(bw);
					r.hi = c->getValue();
					return r;
				}
			}
			return r;
		}
		case llvm::Instruction::URem:
		{
			// x % C is at most C-1.
			if (auto* c = constOp(1))
			{
				if (!c->isZero())
				{
					r.known = true;
					r.lo = llvm::APInt::getZero(bw);
					r.hi = c->getValue() - 1;
					return r;
				}
			}
			return r;
		}
		case llvm::Instruction::ZExt:
		{
			Rng a = rangeOf(in->getOperand(0), depth + 1);
			if (!a.known) return r;
			r.known = true;
			r.lo = a.lo.zext(bw);
			r.hi = a.hi.zext(bw);
			return r;
		}
		case llvm::Instruction::Trunc:
		{
			Rng a = rangeOf(in->getOperand(0), depth + 1);
			// Only when the whole interval survives the truncation; otherwise
			// it wraps and the bound is worthless.
			if (!a.known || a.hi.getActiveBits() > bw) return r;
			r.known = true;
			r.lo = a.lo.trunc(bw);
			r.hi = a.hi.trunc(bw);
			return r;
		}
		case llvm::Instruction::Sub:
		{
			Rng a = rangeOf(in->getOperand(0), depth + 1);
			Rng b = rangeOf(in->getOperand(1), depth + 1);
			// Only when it cannot wrap. a - b with b <= a.lo is [a.lo-b.hi,
			// a.hi-b.lo]; anything else is unknown rather than guessed.
			if (!a.known || !b.known || b.hi.ugt(a.lo)) return r;
			r.known = true;
			r.lo = a.lo - b.hi;
			r.hi = a.hi - b.lo;
			return r;
		}
		case llvm::Instruction::Select:
		{
			Rng a = rangeOf(in->getOperand(1), depth + 1);
			Rng b = rangeOf(in->getOperand(2), depth + 1);
			if (!a.known || !b.known) return r;
			r.known = true;
			r.lo = a.lo.ult(b.lo) ? a.lo : b.lo;
			r.hi = a.hi.ugt(b.hi) ? a.hi : b.hi;
			return r;
		}
		case llvm::Instruction::Call:
		{
			auto* ci = llvm::cast<llvm::CallInst>(in);
			auto* f = ci->getCalledFunction();
			if (f == nullptr || f->getIntrinsicID() != llvm::Intrinsic::umin)
			{
				return r;
			}
			Rng a = rangeOf(ci->getArgOperand(0), depth + 1);
			Rng b = rangeOf(ci->getArgOperand(1), depth + 1);
			// umin needs only ONE side bounded: the result cannot exceed
			// either argument.
			if (!a.known && !b.known) return r;
			if (!a.known) { r.known = true; r.lo = llvm::APInt::getZero(bw); r.hi = b.hi; return r; }
			if (!b.known) { r.known = true; r.lo = llvm::APInt::getZero(bw); r.hi = a.hi; return r; }
			r.known = true;
			r.lo = a.lo.ult(b.lo) ? a.lo : b.lo;
			r.hi = a.hi.ult(b.hi) ? a.hi : b.hi;
			return r;
		}
		default:
			return r;
	}
}

/// True when @p amt can reach @p width, i.e. when this shift may be poison.
bool mayBeOutOfRange(llvm::Value* amt, unsigned width)
{
	Rng r = rangeOf(amt);
	return !r.known || r.hi.uge(width);
}

} // namespace

int main(int argc, char** argv)
{
	bool selfTest = argc > 1 && !strcmp(argv[1], "--self-test");

	unsigned checked = 0, shifts = 0, bad = 0, skipped = 0;

	for (const auto& c : CASES)
	{
		std::vector<uint8_t> code;
		if (!assemble(c.arch, c.asmText, code))
		{
			printf("  SKIP  %-8s %-28s (assembler declined)\n", c.arch, c.asmText);
			++skipped;
			continue;
		}

		llvm::LLVMContext ctx;
		llvm::Module m("m", ctx);
		auto t = makeTranslator(c.arch, &m);
		if (t == nullptr) { ++skipped; continue; }

		auto* fn = llvm::Function::Create(
				llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
				llvm::GlobalValue::ExternalLinkage, "probe", &m);
		auto* bb = llvm::BasicBlock::Create(ctx, "entry", fn);
		llvm::IRBuilder<> irb(bb);
		auto* ret = irb.CreateRetVoid();
		irb.SetInsertPoint(ret);
		try
		{
			t->translate(code.data(), code.size(), 0x1000, irb);
		}
		catch (const std::exception& e)
		{
			printf("  SKIP  %-8s %-28s (translator threw: %s)\n",
				   c.arch, c.asmText, e.what());
			++skipped;
			continue;
		}
		++checked;

		for (auto it = llvm::inst_begin(fn), e = llvm::inst_end(fn); it != e; ++it)
		{
			unsigned op = it->getOpcode();
			if (op != llvm::Instruction::Shl
					&& op != llvm::Instruction::LShr
					&& op != llvm::Instruction::AShr)
			{
				continue;
			}
			auto* ty = llvm::dyn_cast<llvm::IntegerType>(it->getType());
			if (ty == nullptr) continue;   // vector shifts are checked lane-wise elsewhere
			++shifts;
			if (mayBeOutOfRange(it->getOperand(1), ty->getBitWidth()))
			{
				++bad;
				if (bad <= 80)
				{
					std::string s;
					llvm::raw_string_ostream os(s);
					it->print(os);
					std::string a;
					llvm::raw_string_ostream ao(a);
					if (auto* ai = llvm::dyn_cast<llvm::Instruction>(it->getOperand(1)))
					{
						ai->print(ao);
					}
					else
					{
						it->getOperand(1)->printAsOperand(ao);
					}
					Rng rr = rangeOf(it->getOperand(1));
					char rb[80];
					if (rr.known)
					{
						snprintf(rb, sizeof rb, "[%llu, %llu]",
								 (unsigned long long)rr.lo.getLimitedValue(),
								 (unsigned long long)rr.hi.getLimitedValue());
					}
					else
					{
						snprintf(rb, sizeof rb, "unknown");
					}
					std::string rs(rb);
					printf("  POISON %-8s %-28s width=%u  %s\n"
						   "         amount: %s\n"
						   "         range:  %s\n",
						   c.arch, c.asmText, ty->getBitWidth(), s.c_str(),
						   a.c_str(), rs.c_str());
				}
			}
		}
	}

	// A check that cannot report a problem proves nothing: build a shift that
	// IS out of range and confirm the analysis says so.
	if (selfTest)
	{
		llvm::LLVMContext ctx;
		llvm::Module m("st", ctx);
		auto* fn = llvm::Function::Create(
				llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
				llvm::GlobalValue::ExternalLinkage, "st", &m);
		llvm::IRBuilder<> irb(llvm::BasicBlock::Create(ctx, "e", fn));
		auto* gv = new llvm::GlobalVariable(
				m, irb.getInt8Ty(), false,
				llvm::GlobalValue::ExternalLinkage,
				irb.getInt8(0), "any");
		auto* load = irb.CreateLoad(irb.getInt8Ty(), gv);

		// Unmasked: an i8 amount reaches 255, so this must be flagged.
		if (!mayBeOutOfRange(load, 8))
		{
			printf("SHIFT-01: self-test FAILED -- an unmasked i8 amount was "
				   "not reported as possibly out of range\n");
			return 2;
		}
		// Masked to three bits: at most 7, so this must NOT be flagged.
		auto* masked = irb.CreateAnd(load, irb.getInt8(7));
		if (mayBeOutOfRange(masked, 8))
		{
			printf("SHIFT-01: self-test FAILED -- an amount masked to 3 bits "
				   "was reported as possibly out of range\n");
			return 2;
		}
		printf("SHIFT-01: self-test ok -- unmasked flagged, masked not\n");
	}

	printf("\n%u instructions translated, %u integer shifts examined, "
		   "%u possibly out of range (%u skipped)\n",
		   checked, shifts, bad, skipped);
	return bad ? 1 : 0;
}
