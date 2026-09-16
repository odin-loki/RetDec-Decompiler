/**
 * @file scripts/ci/x86_flag_compare.cpp
 * @brief Compares the translator flag model against x86_flag_oracle output.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 */

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "retdec/capstone2llvmir/capstone2llvmir.h"
#include "retdec/llvmir-emul/llvmir_emul.h"

using namespace retdec::capstone2llvmir;
using namespace retdec::llvmir_emul;

struct Row { std::string name; unsigned bits; uint64_t a, b, res; unsigned cf, pf, af, zf, sf, of; };

static const struct { const char* name; const char* bytes; } ENC[] = {
	{"add64", "48 01 c8"},   {"sub64", "48 29 c8"},   {"and64", "48 21 c8"},
	{"or64",  "48 09 c8"},   {"xor64", "48 31 c8"},   {"cmp64", "48 39 c8"},
	{"test64","48 85 c8"},   {"imul64","48 0f af c1"},
	{"add32", "01 c8"},      {"sub32", "29 c8"},      {"imul32","0f af c1"},
	{"neg64", "48 f7 d8"},   {"inc64", "48 ff c0"},   {"dec64", "48 ff c8"},
	{"not64", "48 f7 d0"},
	{"shl64", "48 d3 e0"},   {"shr64", "48 d3 e8"},   {"sar64", "48 d3 f8"},
	{"rol64", "48 d3 c0"},   {"ror64", "48 d3 c8"},
	{"adc64", "48 11 c8"},   {"sbb64", "48 19 c8"},
	{"bt64",  "48 0f a3 c8"},{"bts64", "48 0f ab c8"},
	{"btr64", "48 0f b3 c8"},{"btc64", "48 0f bb c8"},
	{"rcl64", "48 d3 d0"},   {"rcr64", "48 d3 d8"},
	{"bsf64", "48 0f bc c1"},{"bsr64", "48 0f bd c1"},
	{"add8",  "00 c8"},      {"sub8",  "28 c8"},
	{"add16", "66 01 c8"},   {"sub16", "66 29 c8"},
};

static std::vector<uint8_t> hexBytes(const char* s)
{
	std::vector<uint8_t> v;
	while (*s) {
		while (*s == ' ') ++s;
		if (!*s) break;
		v.push_back((uint8_t)strtoul(s, nullptr, 16));
		while (*s && *s != ' ') ++s;
	}
	return v;
}

static void setGv(LlvmIrEmulator& e, llvm::GlobalVariable* gv, uint64_t val)
{
	llvm::GenericValue g = e.getGlobalVariableValue(gv);
	unsigned bw = gv->getValueType()->getPrimitiveSizeInBits();
	g.IntVal = llvm::APInt(bw, val, false, true);
	e.setGlobalVariableValue(gv, g);
}

static uint64_t getGv(LlvmIrEmulator& e, llvm::GlobalVariable* gv)
{
	const llvm::APInt& v = e.getGlobalVariableValue(gv).IntVal;
	return v.getBitWidth() > 64 ? v.trunc(64).getZExtValue() : v.getZExtValue();
}

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "rows.txt";
	FILE* f = fopen(path, "r");
	if (!f) { printf("cannot open %s\n", path); return 2; }

	std::vector<Row> rows;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		Row r; char nm[32], txt[64];
		if (sscanf(line, "%31[^|]|%63[^|]|%u|%llu|%llu|%llu|%u|%u|%u|%u|%u|%u", nm, txt, &r.bits,
				(unsigned long long*)&r.a, (unsigned long long*)&r.b, (unsigned long long*)&r.res,
				&r.cf, &r.pf, &r.af, &r.zf, &r.sf, &r.of) == 12) {
			r.name = nm;
			rows.push_back(r);
		}
	}
	fclose(f);

	unsigned long tried = 0, bad = 0;
	// Flags RetDec is known not to model for a given instruction are reported
	// separately rather than counted as mismatches, so a real disagreement is
	// not buried in them.
	unsigned long unmodelled = 0;

	for (auto& e : ENC) {
		llvm::LLVMContext c;
		llvm::Module m("m", c);
		auto t = Capstone2LlvmIrTranslator::createX86_64(&m);
		LlvmIrEmulator emu(&m);

		auto code = hexBytes(e.bytes);
		auto* fn = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(c), false),
			llvm::GlobalValue::ExternalLinkage, "probe", &m);
		auto* bb = llvm::BasicBlock::Create(c, "entry", fn);
		llvm::IRBuilder<> irb(bb);
		// The terminator has to exist BEFORE translating: _generateIfThen()
		// splits the block at the insert point and needs one. This is what
		// the gtest harness does, and getting it wrong throws "Bad insert
		// point" part way through the shifts.
		auto* ret = irb.CreateRetVoid();
		irb.SetInsertPoint(ret);
		t->translate(code.data(), code.size(), 0x1000, irb);

		struct { const char* n; uint32_t r; } FL[] = {
			{"cf", X86_REG_CF}, {"pf", X86_REG_PF}, {"af", X86_REG_AF},
			{"zf", X86_REG_ZF}, {"sf", X86_REG_SF}, {"of", X86_REG_OF},
		};

		// Which flags the architecture DEFINES, in CF PF AF ZF SF OF order.
		// Comparing against an undefined flag is comparing against whatever
		// this particular CPU happened to leave behind -- IMUL leaves SF, ZF,
		// AF and PF undefined, and the first run of this test reported dozens
		// of "mismatches" that were nothing but that.
		bool def[6] = {true, true, true, true, true, true};
		std::string n = e.name;
		if (n == "and64" || n == "or64" || n == "xor64" || n == "test64")
		{
			def[2] = false; // AF undefined for the logicals
		}
		else if (n == "imul64" || n == "imul32")
		{
			def[1] = def[2] = def[3] = def[4] = false; // only CF and OF
		}
		else if (n == "shl64" || n == "shr64" || n == "sar64")
		{
			def[2] = false; // AF undefined
			def[5] = false; // OF defined only for a count of 1
		}
		else if (n == "rol64" || n == "ror64")
		{
			def[5] = false; // OF defined only for a count of 1
		}
		else if (n == "bt64" || n == "bts64" || n == "btr64" || n == "btc64")
		{
			def[1] = def[2] = def[3] = def[4] = def[5] = false; // only CF
		}
		else if (n == "rcl64" || n == "rcr64")
		{
			// RCL/RCR touch CF and OF only, and OF only for a count of one.
			def[1] = def[2] = def[3] = def[4] = def[5] = false;
		}
		else if (n == "bsf64" || n == "bsr64")
		{
			// Only ZF is defined; the destination is undefined when the
			// source is zero, which is why the result is not compared either.
			def[0] = def[1] = def[2] = def[4] = def[5] = false;
		}
		else if (n == "not64")
		{
			for (int k = 0; k < 6; ++k) def[k] = false; // NOT affects none
		}

		for (auto& r : rows) {
			if (r.name != e.name) continue;
			setGv(emu, t->getRegister(X86_REG_RAX), r.a);
			setGv(emu, t->getRegister(X86_REG_RCX), r.b);
			for (auto& fl : FL) setGv(emu, t->getRegister(fl.r), 0);
			if (e.name == std::string("adc64") || e.name == std::string("sbb64")
					|| e.name == std::string("rcl64") || e.name == std::string("rcr64"))
			{
				setGv(emu, t->getRegister(X86_REG_CF), r.b & 1);
			}
			emu.runFunction(fn, {});

			uint64_t got = getGv(emu, t->getRegister(X86_REG_RAX));
			if (r.bits < 64) got &= (1ULL << r.bits) - 1;
			unsigned g[6] = {
				(unsigned)getGv(emu, t->getRegister(X86_REG_CF)),
				(unsigned)getGv(emu, t->getRegister(X86_REG_PF)),
				(unsigned)getGv(emu, t->getRegister(X86_REG_AF)),
				(unsigned)getGv(emu, t->getRegister(X86_REG_ZF)),
				(unsigned)getGv(emu, t->getRegister(X86_REG_SF)),
				(unsigned)getGv(emu, t->getRegister(X86_REG_OF)),
			};
			unsigned w[6] = {r.cf, r.pf, r.af, r.zf, r.sf, r.of};
			++tried;

			// BSF/BSR leave the destination undefined when the source is
			// zero, so the result is only compared when it is defined.
			bool bsZero = (r.name == "bsf64" || r.name == "bsr64") && r.b == 0;
			bool resBad = (r.name != "cmp64" && r.name != "test64" && r.name != "bt64" && !bsZero)
						  && got != r.res;
			std::string flagsBad;
			for (int k = 0; k < 6; ++k) {
				if (!def[k]) { if (g[k] != w[k]) ++unmodelled; continue; }
				if (g[k] != w[k]) { flagsBad += " "; flagsBad += FL[k].n; }
			}
			if (resBad || !flagsBad.empty()) {
				++bad;
				if (bad <= 4000) {
					printf("%-7s a=%016llx b=%016llx  res got=%016llx want=%016llx%s\n",
						   r.name.c_str(), (unsigned long long)r.a, (unsigned long long)r.b,
						   (unsigned long long)got, (unsigned long long)r.res,
						   resBad ? "  RESULT" : "");
					if (!flagsBad.empty()) {
						printf("          flags differ:%s   got cf%u pf%u af%u zf%u sf%u of%u"
							   "  want cf%u pf%u af%u zf%u sf%u of%u\n",
							   flagsBad.c_str(), g[0],g[1],g[2],g[3],g[4],g[5],
							   w[0],w[1],w[2],w[3],w[4],w[5]);
					}
				}
			}
		}
	}
	printf("\n%lu comparisons, %lu mismatches (%lu unmodelled)\n", tried, bad, unmodelled);
	return 0;
}
