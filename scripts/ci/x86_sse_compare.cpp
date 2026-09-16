/**
 * @file scripts/ci/x86_sse_compare.cpp
 * @brief Compares the translator's XMM-reading, GPR-writing instructions
 *        against x86_sse_oracle output.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 */

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include "retdec/capstone2llvmir/capstone2llvmir.h"
#include "retdec/llvmir-emul/llvmir_emul.h"

using namespace retdec::capstone2llvmir;
using namespace retdec::llvmir_emul;

/* The destination GPR is seeded with this in the oracle, so a comparison --
   which writes no GPR at all -- must leave it untouched, and a 32-bit write
   must clear the top half rather than leave it. */
static const uint64_t GPR_SEED = 0xdeadbeefdeadbeefULL;

struct Row {
	std::string name;
	uint64_t x0lo, x0hi, x1lo, x1hi, gpr;
	unsigned cf, pf, af, zf, sf, of;
};

static const struct { const char* name; const char* bytes; } ENC[] = {
	{"ucomisd",    "66 0f 2e c1"}, {"ucomiss",    "0f 2e c1"},
	{"comisd",     "66 0f 2f c1"}, {"comiss",     "0f 2f c1"},
	{"vucomisd",   "c5 f9 2e c1"}, {"vucomiss",   "c5 f8 2e c1"},
	{"vcomisd",    "c5 f9 2f c1"}, {"vcomiss",    "c5 f8 2f c1"},
	{"movmskps",   "0f 50 c1"},    {"movmskpd",   "66 0f 50 c1"},
	{"vmovmskps",  "c5 f8 50 c1"}, {"vmovmskpd",  "c5 f9 50 c1"},
	{"cvtsd2si",   "f2 0f 2d c1"}, {"cvtss2si",   "f3 0f 2d c1"},
	{"cvttsd2si",  "f2 0f 2c c1"}, {"cvttss2si",  "f3 0f 2c c1"},
	{"vcvtsd2si",  "c5 fb 2d c1"}, {"vcvtss2si",  "c5 fa 2d c1"},
	{"vcvttsd2si", "c5 fb 2c c1"}, {"vcvttss2si", "c5 fa 2c c1"},
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

static void setWide(LlvmIrEmulator& e, llvm::GlobalVariable* gv,
                    uint64_t lo, uint64_t hi)
{
	llvm::GenericValue g = e.getGlobalVariableValue(gv);
	unsigned bw = gv->getValueType()->getPrimitiveSizeInBits();
	uint64_t words[2] = {lo, hi};
	g.IntVal = llvm::APInt(bw, llvm::ArrayRef<uint64_t>(words, bw > 64 ? 2 : 1));
	e.setGlobalVariableValue(gv, g);
}

static uint64_t getGv(LlvmIrEmulator& e, llvm::GlobalVariable* gv)
{
	const llvm::APInt& v = e.getGlobalVariableValue(gv).IntVal;
	return v.getBitWidth() > 64 ? v.trunc(64).getZExtValue() : v.getZExtValue();
}

static bool endsWith(const std::string& s, const char* p)
{
	size_t n = strlen(p);
	return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "srows.txt";
	FILE* f = fopen(path, "r");
	if (!f) { printf("cannot open %s\n", path); return 2; }

	std::vector<Row> rows;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		Row r; char nm[32];
		if (sscanf(line, "%31[^|]|%llu|%llu|%llu|%llu|%llu|%u|%u|%u|%u|%u|%u",
				nm,
				(unsigned long long*)&r.x0lo, (unsigned long long*)&r.x0hi,
				(unsigned long long*)&r.x1lo, (unsigned long long*)&r.x1hi,
				(unsigned long long*)&r.gpr,
				&r.cf, &r.pf, &r.af, &r.zf, &r.sf, &r.of) == 12) {
			r.name = nm;
			rows.push_back(r);
		}
	}
	fclose(f);

	unsigned long tried = 0, bad = 0, unmodelled = 0, pseudo = 0;
	std::map<std::string, unsigned long> perInsn;

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
		auto* ret = irb.CreateRetVoid();
		irb.SetInsertPoint(ret);
		t->translate(code.data(), code.size(), 0x1000, irb);

		if (llvm::verifyFunction(*fn, &llvm::errs())) {
			printf("%-11s INVALID IR\n", e.name);
			++bad;
			continue;
		}

		// An instruction that fell through to a pseudo-assembly call is not
		// translated wrongly, it is not translated at all. Counting it as a
		// mismatch would bury it among real disagreements, so it is named
		// once and its rows are skipped -- the coverage checks are what
		// report it.
		bool isPseudo = false;
		for (auto& bbi : *fn) {
			for (auto& ins : bbi) {
				auto* call = llvm::dyn_cast<llvm::CallInst>(&ins);
				if (call && call->getCalledFunction()
						&& call->getCalledFunction()->getName().starts_with("__asm_")) {
					isPseudo = true;
				}
			}
		}
		if (isPseudo) {
			printf("%-11s NOT TRANSLATED -- falls through to pseudo-assembly\n", e.name);
			++pseudo;
			continue;
		}

		struct { const char* n; uint32_t r; } FL[] = {
			{"cf", X86_REG_CF}, {"pf", X86_REG_PF}, {"af", X86_REG_AF},
			{"zf", X86_REG_ZF}, {"sf", X86_REG_SF}, {"of", X86_REG_OF},
		};

		std::string n = e.name;
		bool isCompare = endsWith(n, "comisd") || endsWith(n, "comiss");

		for (auto& r : rows) {
			if (r.name != e.name) continue;
			setWide(emu, t->getRegister(X86_REG_XMM0), r.x0lo, r.x0hi);
			setWide(emu, t->getRegister(X86_REG_XMM1), r.x1lo, r.x1hi);
			setGv(emu, t->getRegister(X86_REG_RAX), GPR_SEED);
			for (auto& fl : FL) setGv(emu, t->getRegister(fl.r), 0);
			emu.runFunction(fn, {});

			uint64_t got = getGv(emu, t->getRegister(X86_REG_RAX));
			unsigned g[6];
			for (int k = 0; k < 6; ++k) {
				g[k] = (unsigned)getGv(emu, t->getRegister(FL[k].r));
			}
			unsigned w[6] = {r.cf, r.pf, r.af, r.zf, r.sf, r.of};
			++tried;

			// Every flag is defined for all three families: the comparisons
			// set ZF, PF and CF and CLEAR OF, SF and AF; the mask
			// extractions and the conversions touch none, which both sides
			// express by leaving the cleared value in place.
			std::string flagsBad;
			for (int k = 0; k < 6; ++k) {
				if (g[k] != w[k]) { flagsBad += " "; flagsBad += FL[k].n; }
			}
			bool resBad = got != r.gpr;
			if (resBad || !flagsBad.empty()) {
				++bad;
				++perInsn[e.name];
				if (bad <= 30) {
					printf("%-11s xmm0=%016llx_%016llx xmm1=%016llx_%016llx\n",
						   r.name.c_str(),
						   (unsigned long long)r.x0hi, (unsigned long long)r.x0lo,
						   (unsigned long long)r.x1hi, (unsigned long long)r.x1lo);
					if (resBad) {
						printf("            rax got=%016llx want=%016llx%s\n",
							   (unsigned long long)got, (unsigned long long)r.gpr,
							   isCompare ? "  (a comparison writes no GPR)" : "");
					}
					if (!flagsBad.empty()) {
						printf("            flags differ:%s   got cf%u pf%u af%u zf%u sf%u of%u"
							   "  want cf%u pf%u af%u zf%u sf%u of%u\n",
							   flagsBad.c_str(), g[0],g[1],g[2],g[3],g[4],g[5],
							   w[0],w[1],w[2],w[3],w[4],w[5]);
					}
				}
			}
		}
	}
	if (bad) {
		printf("\nby instruction:");
		for (auto& e : ENC) {
			if (perInsn[e.name]) printf("  %s=%lu", e.name, perInsn[e.name]);
		}
		printf("\n");
	}
	printf("\n%lu comparisons, %lu mismatches (%lu unmodelled, %lu untranslated forms)\n",
		   tried, bad, unmodelled, pseudo);
	return 0;
}
