/**
 * @file scripts/ci/x86_wide_compare.cpp
 * @brief Compares the translator's MUL/IMUL/DIV/IDIV/SHLD/SHRD against
 *        x86_wide_oracle output.
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
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include "retdec/capstone2llvmir/capstone2llvmir.h"
#include "retdec/llvmir-emul/llvmir_emul.h"

using namespace retdec::capstone2llvmir;
using namespace retdec::llvmir_emul;

struct Row {
	std::string name;
	unsigned bits;
	uint64_t a, b, d, resA, resD;
	unsigned cf, pf, af, zf, sf, of;
};

static const struct { const char* name; const char* bytes; } ENC[] = {
	{"mul8",   "f6 e1"},       {"mul16",  "66 f7 e1"},
	{"mul32",  "f7 e1"},       {"mul64",  "48 f7 e1"},
	{"imul8",  "f6 e9"},       {"imul16", "66 f7 e9"},
	{"imul32", "f7 e9"},       {"imul64", "48 f7 e9"},
	{"div8",   "f6 f1"},       {"div16",  "66 f7 f1"},
	{"div32",  "f7 f1"},       {"div64",  "48 f7 f1"},
	{"idiv8",  "f6 f9"},       {"idiv16", "66 f7 f9"},
	{"idiv32", "f7 f9"},       {"idiv64", "48 f7 f9"},
	{"shld16", "66 0f a5 c8"}, {"shld32", "0f a5 c8"},
	{"shld64", "48 0f a5 c8"},
	{"shrd16", "66 0f ad c8"}, {"shrd32", "0f ad c8"},
	{"shrd64", "48 0f ad c8"},
	{"shl32",  "d3 e0"},       {"shr32",  "d3 e8"},
	{"sar32",  "d3 f8"},       {"rol32",  "d3 c0"},
	{"ror32",  "d3 c8"},       {"rcl32",  "d3 d0"},
	{"rcr32",  "d3 d8"},
	{"shl8",   "d2 e0"},       {"shr8",   "d2 e8"},
	{"rol8",   "d2 c0"},       {"ror8",   "d2 c8"},
	{"shl16",  "66 d3 e0"},    {"sar16",  "66 d3 f8"},
	{"rol16",  "66 d3 c0"},
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

static bool startsWith(const std::string& s, const char* p)
{
	return s.compare(0, strlen(p), p) == 0;
}

/* The single-operand shifts and rotates: a three-letter mnemonic followed by
   the operand width. SHLD and SHRD are deliberately not in this set -- they
   take their count the same way but do not read the carry. */
static bool isSingleShift(const std::string& n)
{
	static const char* P[] = {"shl", "shr", "sar", "rol", "ror", "rcl", "rcr"};
	if (n.size() < 4 || !isdigit((unsigned char)n[3])) return false;
	for (const char* p : P) if (startsWith(n, p)) return true;
	return false;
}

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "wrows.txt";
	FILE* f = fopen(path, "r");
	if (!f) { printf("cannot open %s\n", path); return 2; }

	std::vector<Row> rows;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		Row r; char nm[32];
		if (sscanf(line, "%31[^|]|%u|%llu|%llu|%llu|%llu|%llu|%u|%u|%u|%u|%u|%u",
				nm, &r.bits,
				(unsigned long long*)&r.a, (unsigned long long*)&r.b,
				(unsigned long long*)&r.d, (unsigned long long*)&r.resA,
				(unsigned long long*)&r.resD,
				&r.cf, &r.pf, &r.af, &r.zf, &r.sf, &r.of) == 13) {
			r.name = nm;
			rows.push_back(r);
		}
	}
	fclose(f);

	unsigned long tried = 0, bad = 0, unmodelled = 0;
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
		// The terminator has to exist before translating: _generateIfThen()
		// splits the block at the insert point and needs one.
		auto* ret = irb.CreateRetVoid();
		irb.SetInsertPoint(ret);
		t->translate(code.data(), code.size(), 0x1000, irb);

		// The emulator does not type-check. It will happily evaluate IR that
		// no verifier would accept and return a plausible number, so the
		// verifier runs first -- this is what turned a silent pass into a
		// failure once already.
		if (llvm::verifyFunction(*fn, &llvm::errs())) {
			printf("%-7s INVALID IR\n", e.name);
			++bad;
			continue;
		}

		std::string n = e.name;
		// Which flags the architecture DEFINES.  MUL and IMUL define only CF
		// and OF; DIV and IDIV define nothing at all; SHLD and SHRD define
		// everything but AF, and OF only for a count of one.
		bool def[6] = {true, true, true, true, true, true};
		if (startsWith(n, "mul") || startsWith(n, "imul")) {
			def[1] = def[2] = def[3] = def[4] = false;
		} else if (startsWith(n, "div") || startsWith(n, "idiv")) {
			for (int k = 0; k < 6; ++k) def[k] = false;
		} else if (startsWith(n, "rol") || startsWith(n, "ror")
				|| startsWith(n, "rcl") || startsWith(n, "rcr")) {
			// The rotates touch CF and OF and nothing else. SF, ZF, AF and
			// PF are not "undefined" here, they are UNAFFECTED, so they are
			// still compared: both sides start from zero and a translator
			// that writes one has invented a flag change.
			def[1] = def[2] = false;
		} else {
			def[2] = false;   // AF undefined
		}

		struct { const char* n; uint32_t r; } FL[] = {
			{"cf", X86_REG_CF}, {"pf", X86_REG_PF}, {"af", X86_REG_AF},
			{"zf", X86_REG_ZF}, {"sf", X86_REG_SF}, {"of", X86_REG_OF},
		};

		for (auto& r : rows) {
			if (r.name != e.name) continue;
			setGv(emu, t->getRegister(X86_REG_RAX), r.a);
			setGv(emu, t->getRegister(X86_REG_RCX), r.b);
			setGv(emu, t->getRegister(X86_REG_RDX), r.d);
			for (auto& fl : FL) setGv(emu, t->getRegister(fl.r), 0);
			// Bit 8 of the count word carries the incoming CF, on both
			// sides. RCL and RCR rotate THROUGH the carry so it is a real
			// input for them; for the rest it matters because a count that
			// masks to zero leaves CF alone, and "alone" can only be checked
			// if both sides started from the same value. Seeding it on one
			// side only produced eight rol/ror "mismatches" that were the
			// harness disagreeing with itself.
			if (isSingleShift(n)) {
				setGv(emu, t->getRegister(X86_REG_CF), (r.b >> 8) & 1);
			}
			emu.runFunction(fn, {});

			uint64_t gotA = getGv(emu, t->getRegister(X86_REG_RAX));
			uint64_t gotD = getGv(emu, t->getRegister(X86_REG_RDX));
			unsigned g[6];
			for (int k = 0; k < 6; ++k) {
				g[k] = (unsigned)getGv(emu, t->getRegister(FL[k].r));
			}
			unsigned w[6] = {r.cf, r.pf, r.af, r.zf, r.sf, r.of};
			++tried;

			bool rowDef[6];
			for (int k = 0; k < 6; ++k) rowDef[k] = def[k];
			// OF is defined only for a count of one -- for every shift and
			// every rotate, single- or double-precision. A count of zero
			// changes no flag at all, which both sides express by leaving
			// the cleared value in place.
			{
				unsigned cnt = (unsigned)(r.b & 0xff)
						& (r.bits == 64 ? 63u : 31u);
				if (cnt != 1) rowDef[5] = false;
				// A shift by at least the operand width leaves CF undefined
				// at 8 and 16 bits; the oracle does not ask for those, but
				// the rule is stated here rather than assumed.
				if (cnt >= r.bits && (startsWith(n, "shl")
						|| startsWith(n, "shr") || startsWith(n, "sar"))) {
					rowDef[0] = false;
				}
			}

			bool resBad = gotA != r.resA || gotD != r.resD;
			std::string flagsBad;
			for (int k = 0; k < 6; ++k) {
				if (!rowDef[k]) { if (g[k] != w[k]) ++unmodelled; continue; }
				if (g[k] != w[k]) { flagsBad += " "; flagsBad += FL[k].n; }
			}
			if (resBad || !flagsBad.empty()) {
				++bad;
				++perInsn[e.name];
				if (bad <= 40) {
					printf("%-7s a=%016llx b=%016llx d=%016llx\n",
						   r.name.c_str(), (unsigned long long)r.a,
						   (unsigned long long)r.b, (unsigned long long)r.d);
					if (resBad) {
						printf("          rax got=%016llx want=%016llx   "
							   "rdx got=%016llx want=%016llx  RESULT\n",
							   (unsigned long long)gotA, (unsigned long long)r.resA,
							   (unsigned long long)gotD, (unsigned long long)r.resD);
					}
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
	if (bad) {
		printf("\nby instruction:");
		for (auto& e : ENC) {
			if (perInsn[e.name]) printf("  %s=%lu", e.name, perInsn[e.name]);
		}
		printf("\n");
	}
	printf("\n%lu comparisons, %lu mismatches (%lu unmodelled)\n",
		   tried, bad, unmodelled);
	return 0;
}
