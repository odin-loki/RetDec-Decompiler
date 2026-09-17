/**
 * @file scripts/ci/x86_vec_compare.cpp
 * @brief Compares the translator's XMM-to-XMM instructions against
 *        x86_vec_oracle output.
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

struct Row { std::string name; uint64_t x0lo, x0hi, x1lo, x1hi, rlo, rhi; };

static const struct { const char* name; const char* bytes; } ENC[] = {
	{"psllw", "66 0f f1 c1"}, {"pslld", "66 0f f2 c1"}, {"psllq", "66 0f f3 c1"},
	{"psrlw", "66 0f d1 c1"}, {"psrld", "66 0f d2 c1"}, {"psrlq", "66 0f d3 c1"},
	{"psraw", "66 0f e1 c1"}, {"psrad", "66 0f e2 c1"},
	{"pmovsxbw", "66 0f 38 20 c1"}, {"pmovsxbd", "66 0f 38 21 c1"},
	{"pmovsxbq", "66 0f 38 22 c1"}, {"pmovsxwd", "66 0f 38 23 c1"},
	{"pmovsxwq", "66 0f 38 24 c1"}, {"pmovsxdq", "66 0f 38 25 c1"},
	{"pmovzxbw", "66 0f 38 30 c1"}, {"pmovzxbd", "66 0f 38 31 c1"},
	{"pmovzxbq", "66 0f 38 32 c1"}, {"pmovzxwd", "66 0f 38 33 c1"},
	{"pmovzxwq", "66 0f 38 34 c1"}, {"pmovzxdq", "66 0f 38 35 c1"},
	{"paddsb",  "66 0f ec c1"}, {"paddsw",  "66 0f ed c1"},
	{"paddusb", "66 0f dc c1"}, {"paddusw", "66 0f dd c1"},
	{"psubsb",  "66 0f e8 c1"}, {"psubsw",  "66 0f e9 c1"},
	{"psubusb", "66 0f d8 c1"}, {"psubusw", "66 0f d9 c1"},
	{"pmullw",  "66 0f d5 c1"}, {"pmulhw",  "66 0f e5 c1"},
	{"pmulhuw", "66 0f e4 c1"}, {"pmulld",  "66 0f 38 40 c1"},
	{"pmuludq", "66 0f f4 c1"}, {"pmuldq",  "66 0f 38 28 c1"},
	{"packsswb", "66 0f 63 c1"}, {"packssdw", "66 0f 6b c1"},
	{"packuswb", "66 0f 67 c1"}, {"packusdw", "66 0f 38 2b c1"},
	{"pmaddwd",  "66 0f f5 c1"}, {"psadbw",   "66 0f f6 c1"},
	{"pshufb",   "66 0f 38 00 c1"},
	{"phaddw",   "66 0f 38 01 c1"}, {"phaddd",   "66 0f 38 02 c1"},
	{"phaddsw",  "66 0f 38 03 c1"}, {"phsubw",   "66 0f 38 05 c1"},
	{"phsubd",   "66 0f 38 06 c1"}, {"phsubsw",  "66 0f 38 07 c1"},
	{"pabsb",    "66 0f 38 1c c1"}, {"pabsw",    "66 0f 38 1d c1"},
	{"pabsd",    "66 0f 38 1e c1"},
	{"psignb",   "66 0f 38 08 c1"}, {"psignw",   "66 0f 38 09 c1"},
	{"psignd",   "66 0f 38 0a c1"},
	{"pmulhrsw", "66 0f 38 0b c1"}, {"pmaddubsw", "66 0f 38 04 c1"},
	{"phminposuw", "66 0f 38 41 c1"},
	{"pshufd_1b",  "66 0f 70 c1 1b"}, {"pshufd_4e",  "66 0f 70 c1 4e"},
	{"pshufd_c6",  "66 0f 70 c1 c6"},
	{"pshufhw_1b", "f3 0f 70 c1 1b"}, {"pshufhw_c6", "f3 0f 70 c1 c6"},
	{"pshuflw_1b", "f2 0f 70 c1 1b"}, {"pshuflw_c6", "f2 0f 70 c1 c6"},
	{"pblendw_a5", "66 0f 3a 0e c1 a5"}, {"pblendw_0f", "66 0f 3a 0e c1 0f"},
	{"blendps_09", "66 0f 3a 0c c1 09"}, {"blendps_06", "66 0f 3a 0c c1 06"},
	{"blendpd_01", "66 0f 3a 0d c1 01"}, {"blendpd_02", "66 0f 3a 0d c1 02"},
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

static void setWide(LlvmIrEmulator& e, llvm::GlobalVariable* gv,
                    uint64_t lo, uint64_t hi)
{
	llvm::GenericValue g = e.getGlobalVariableValue(gv);
	unsigned bw = gv->getValueType()->getPrimitiveSizeInBits();
	uint64_t words[2] = {lo, hi};
	g.IntVal = llvm::APInt(bw, llvm::ArrayRef<uint64_t>(words, bw > 64 ? 2 : 1));
	e.setGlobalVariableValue(gv, g);
}

static void getWide(LlvmIrEmulator& e, llvm::GlobalVariable* gv,
                    uint64_t& lo, uint64_t& hi)
{
	const llvm::APInt& v = e.getGlobalVariableValue(gv).IntVal;
	lo = v.extractBits(64, 0).getZExtValue();
	hi = v.getBitWidth() > 64 ? v.extractBits(64, 64).getZExtValue() : 0;
}

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "vrows.txt";
	FILE* f = fopen(path, "r");
	if (!f) { printf("cannot open %s\n", path); return 2; }

	std::vector<Row> rows;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		Row r; char nm[32];
		if (sscanf(line, "%31[^|]|%llu|%llu|%llu|%llu|%llu|%llu", nm,
				(unsigned long long*)&r.x0lo, (unsigned long long*)&r.x0hi,
				(unsigned long long*)&r.x1lo, (unsigned long long*)&r.x1hi,
				(unsigned long long*)&r.rlo, (unsigned long long*)&r.rhi) == 7) {
			r.name = nm;
			rows.push_back(r);
		}
	}
	fclose(f);

	unsigned long tried = 0, bad = 0, pseudo = 0, misencoded = 0;
	std::map<std::string, unsigned long> perInsn;

	for (auto& e : ENC) {
		llvm::LLVMContext c;
		llvm::Module m("m", c);
		auto t = Capstone2LlvmIrTranslator::createX86_64(&m);
		LlvmIrEmulator emu(&m);

		auto code = hexBytes(e.bytes);

		// The bytes are hand-written, so they are checked rather than
		// trusted: a wrong ModRM would quietly test a different instruction
		// against this one's expected answers and report a translator bug
		// that is really a typo here.
		{
			csh h;
			if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) == CS_ERR_OK) {
				cs_insn* ins = nullptr;
				size_t n = cs_disasm(h, code.data(), code.size(), 0x1000, 1, &ins);
				// The immediate forms are named "<mnemonic>_<imm>", because
				// the control is part of the encoding and cannot vary per
				// row. Compare against the part before the underscore.
				std::string want(e.name);
				size_t us = want.find('_');
				if (us != std::string::npos) want.resize(us);
				if (n != 1 || std::string(ins->mnemonic) != want) {
					printf("%-11s MISENCODED -- these bytes disassemble to '%s'\n",
						   e.name, n == 1 ? ins->mnemonic : "<nothing>");
					++misencoded;
				}
				if (n) cs_free(ins, n);
				cs_close(&h);
			}
		}

		auto* fn = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(c), false),
			llvm::GlobalValue::ExternalLinkage, "probe", &m);
		auto* bb = llvm::BasicBlock::Create(c, "entry", fn);
		llvm::IRBuilder<> irb(bb);
		auto* ret = irb.CreateRetVoid();
		irb.SetInsertPoint(ret);
		t->translate(code.data(), code.size(), 0x1000, irb);

		if (llvm::verifyFunction(*fn, &llvm::errs())) {
			printf("%-9s INVALID IR\n", e.name);
			++bad;
			continue;
		}

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
			printf("%-9s NOT TRANSLATED -- falls through to pseudo-assembly\n", e.name);
			++pseudo;
			continue;
		}

		for (auto& r : rows) {
			if (r.name != e.name) continue;
			setWide(emu, t->getRegister(X86_REG_XMM0), r.x0lo, r.x0hi);
			setWide(emu, t->getRegister(X86_REG_XMM1), r.x1lo, r.x1hi);
			emu.runFunction(fn, {});

			uint64_t lo = 0, hi = 0;
			getWide(emu, t->getRegister(X86_REG_XMM0), lo, hi);
			++tried;
			if (lo != r.rlo || hi != r.rhi) {
				++bad;
				++perInsn[e.name];
				if (bad <= 24) {
					printf("%-9s xmm0=%016llx_%016llx xmm1=%016llx_%016llx\n"
						   "          got=%016llx_%016llx want=%016llx_%016llx\n",
						   r.name.c_str(),
						   (unsigned long long)r.x0hi, (unsigned long long)r.x0lo,
						   (unsigned long long)r.x1hi, (unsigned long long)r.x1lo,
						   (unsigned long long)hi, (unsigned long long)lo,
						   (unsigned long long)r.rhi, (unsigned long long)r.rlo);
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
	printf("\n%lu comparisons, %lu mismatches (%lu untranslated forms, %lu misencoded)\n",
		   tried, bad, pseudo, misencoded);
	return 0;
}
