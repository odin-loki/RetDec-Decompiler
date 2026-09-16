/**
 * PSEUDO-01 driver -- how many of the instructions in a binary come out of
 * src/capstone2llvmir as a pseudo-assembly call rather than as semantics.
 *
 * COV-01 answers a different question: whether the dispatch table has a
 * function pointer for an instruction id. That is an upper bound, and on
 * ARM64 it is a loose one, because several translators begin with
 *
 *     if (ifVectorGeneratePseudo(i, ai, irb)) { return; }
 *
 * and emit exactly the __asm_* call a nullptr entry would have emitted. An
 * instruction can be "covered" by COV-01 and produce nothing but a call.
 *
 * This asks the translator instead of the table: translate each instruction
 * into a throwaway function and look for a call the translator itself
 * identifies as pseudo-assembly, via isPseudoAsmFunctionCall().
 *
 * Output, one line per instruction:
 *
 *     <capstone id> <pseudo-asm function name, or "-"> <mnemonic>
 *
 * The function name matters. Not every pseudo-assembly call is a gap: the
 * product declares two of them -- `__asm_hlt` and `__asm_rep_stosq_memset` --
 * as intended models rather than unimplemented semantics (see
 * `isAsmIntrinsicName()` in src/retdec/semantic_recovery_export.cpp), and a
 * `rep stosq` modelled as a memset is a better answer than a loop, not a
 * worse one. Printing the name lets the caller tell those apart from
 * `__asm_rlwinm` instead of the gate deciding for it.
 *
 * Usage: pseudo_asm_probe BINARY ARCH OFFSET:SIZE
 *        ARCH is one of x86_64 arm arm_thumb arm64 mips powerpc
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "retdec/capstone2llvmir/capstone2llvmir.h"

using namespace retdec::capstone2llvmir;

namespace {

struct ArchSpec
{
	cs_arch arch;
	cs_mode basic;
	cs_mode extra;
};

bool archFor(const std::string& name, ArchSpec& out)
{
	if (name == "x86_64")    { out = {CS_ARCH_X86,   CS_MODE_64,    CS_MODE_LITTLE_ENDIAN}; return true; }
	if (name == "arm")       { out = {CS_ARCH_ARM,   CS_MODE_ARM,   CS_MODE_LITTLE_ENDIAN}; return true; }
	if (name == "arm_thumb") { out = {CS_ARCH_ARM,   CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN}; return true; }
	if (name == "arm64")     { out = {CS_ARCH_ARM64, CS_MODE_ARM,   CS_MODE_LITTLE_ENDIAN}; return true; }
	if (name == "mips")      { out = {CS_ARCH_MIPS,  CS_MODE_MIPS32, CS_MODE_BIG_ENDIAN};   return true; }
	if (name == "powerpc")   { out = {CS_ARCH_PPC,   CS_MODE_32,    CS_MODE_BIG_ENDIAN};    return true; }
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::fprintf(stderr, "usage: %s BINARY ARCH OFFSET:SIZE\n", argv[0]);
		return 2;
	}

	ArchSpec spec{};
	if (!archFor(argv[2], spec))
	{
		std::fprintf(stderr, "PSEUDO-01: unknown arch %s\n", argv[2]);
		return 2;
	}

	long off = 0, sz = 0;
	if (std::sscanf(argv[3], "%ld:%ld", &off, &sz) != 2 || sz <= 0)
	{
		std::fprintf(stderr, "PSEUDO-01: bad region %s\n", argv[3]);
		return 2;
	}

	std::FILE* f = std::fopen(argv[1], "rb");
	if (!f)
	{
		std::fprintf(stderr, "PSEUDO-01: cannot open %s\n", argv[1]);
		return 2;
	}
	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	std::fseek(f, off, SEEK_SET);
	size_t got = std::fread(buf.data(), 1, buf.size(), f);
	std::fclose(f);
	buf.resize(got);

	llvm::LLVMContext ctx;
	llvm::Module mod("pseudo-asm-probe", ctx);

	auto t = Capstone2LlvmIrTranslator::createArch(spec.arch, &mod, spec.basic, spec.extra);
	if (t == nullptr)
	{
		std::fprintf(stderr, "PSEUDO-01: could not create translator for %s\n", argv[2]);
		return 2;
	}
	// Without this the dispatch throws on an unhandled instruction instead of
	// emitting the pseudo-asm call, and the pseudo-asm call is the thing being
	// counted.
	t->setIgnoreUnhandledInstructions(true);
	t->setIgnoreUnexpectedOperands(true);

	auto* fncTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);

	const uint8_t* bytes = buf.data();
	std::size_t size = buf.size();
	retdec::common::Address addr(0x1000);

	while (size > 0)
	{
		// A fresh function per instruction, because a translation may create
		// basic blocks of its own (generateIfThen and friends) and counting
		// within one block would miss a call placed in a new one.
		auto* fnc = llvm::Function::Create(
				fncTy, llvm::GlobalValue::ExternalLinkage, "probe", &mod);
		auto* bb = llvm::BasicBlock::Create(ctx, "entry", fnc);
		llvm::IRBuilder<> irb(bb);

		const uint8_t* before = bytes;
		Capstone2LlvmIrTranslator::TranslationResultOne res;
		bool threw = false;
		try
		{
			res = t->translateOne(bytes, size, addr, irb);
		}
		catch (const std::exception&)
		{
			threw = true;
		}

		if (threw || res.failed())
		{
			// Undecodable: step one byte and say nothing about it, which is
			// what COV-01's `skipped` column does.
			if (bytes == before)
			{
				++bytes;
				--size;
				addr += 1;
			}
			fnc->eraseFromParent();
			if (res.capstoneInsn)
			{
				cs_free(res.capstoneInsn, 1);
			}
			continue;
		}

		std::string pseudoName;
		for (auto& b : *fnc)
		{
			for (auto& ins : b)
			{
				auto* call = llvm::dyn_cast<llvm::CallInst>(&ins);
				if (call && t->isPseudoAsmFunctionCall(call))
				{
					auto* callee = call->getCalledFunction();
					pseudoName = callee && callee->hasName()
							? callee->getName().str()
							: "__asm_unnamed";
					break;
				}
			}
			if (!pseudoName.empty())
			{
				break;
			}
		}

		std::printf("%u %s %s\n",
				res.capstoneInsn->id,
				pseudoName.empty() ? "-" : pseudoName.c_str(),
				res.capstoneInsn->mnemonic);

		cs_free(res.capstoneInsn, 1);
		fnc->eraseFromParent();
	}

	return 0;
}
