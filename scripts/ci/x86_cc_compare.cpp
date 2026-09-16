/**
 * @file scripts/ci/x86_cc_compare.cpp
 * @brief Compares the translator condition codes against x86_cc_oracle output.
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

struct Row { std::string name; unsigned f[6]; unsigned res; };

static const struct { const char* name; const char* bytes; } ENC[] = {
	{"seto",  "0f 90 c0"}, {"setno", "0f 91 c0"}, {"setb",  "0f 92 c0"}, {"setae", "0f 93 c0"},
	{"sete",  "0f 94 c0"}, {"setne", "0f 95 c0"}, {"setbe", "0f 96 c0"}, {"seta",  "0f 97 c0"},
	{"sets",  "0f 98 c0"}, {"setns", "0f 99 c0"}, {"setp",  "0f 9a c0"}, {"setnp", "0f 9b c0"},
	{"setl",  "0f 9c c0"}, {"setge", "0f 9d c0"}, {"setle", "0f 9e c0"}, {"setg",  "0f 9f c0"},
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
	g.IntVal = llvm::APInt(gv->getValueType()->getPrimitiveSizeInBits(), val, false, true);
	e.setGlobalVariableValue(gv, g);
}

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "rows.txt";
	FILE* f = fopen(path, "r");
	if (!f) { printf("cannot open %s\n", path); return 2; }

	std::vector<Row> rows;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		Row r; char nm[32];
		if (sscanf(line, "%31[^|]|%u|%u|%u|%u|%u|%u|%u", nm,
				&r.f[0],&r.f[1],&r.f[2],&r.f[3],&r.f[4],&r.f[5], &r.res) == 8) {
			r.name = nm;
			rows.push_back(r);
		}
	}
	fclose(f);

	const uint32_t FLREG[6] = {X86_REG_CF, X86_REG_PF, X86_REG_AF,
							   X86_REG_ZF, X86_REG_SF, X86_REG_OF};
	static const char* FLN[6] = {"cf","pf","af","zf","sf","of"};

	unsigned long tried = 0, bad = 0;
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

		for (auto& r : rows) {
			if (r.name != e.name) continue;
			setGv(emu, t->getRegister(X86_REG_RAX), 0);
			for (int k = 0; k < 6; ++k) setGv(emu, t->getRegister(FLREG[k]), r.f[k]);
			emu.runFunction(fn, {});

			uint64_t got = emu.getGlobalVariableValue(t->getRegister(X86_REG_RAX))
							   .IntVal.trunc(64).getZExtValue() & 1;
			++tried;
			if (got != r.res) {
				++bad;
				if (bad <= 60) {
					printf("%-6s", r.name.c_str());
					for (int k = 0; k < 6; ++k) printf(" %s=%u", FLN[k], r.f[k]);
					printf("   got=%llu want=%u\n", (unsigned long long)got, r.res);
				}
			}
		}
	}
	printf("\n%lu comparisons, %lu mismatches\n", tried, bad);
	return 0;
}
