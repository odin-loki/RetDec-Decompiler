/**
* @file tests/llvmir2hll/llvm/llvmir2bir_converter/irreducible_cycle_splitter_tests.cpp
* @brief Tests for the @c irreducible_cycle_splitter module.
* @copyright (c) 2026 Avast Software, licensed under the MIT license
*/

#include <memory>

#include <gtest/gtest.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>

#include "retdec/llvmir2hll/llvm/llvmir2bir_converter/irreducible_cycle_splitter.h"

namespace retdec {
namespace llvmir2hll {
namespace tests {

namespace {

// A cycle entered both at %a and at %b; each entry starts an inner cycle
// (%a -> %m -> %a and %b -> %m -> %b) and the paths meet at %m, the shape of
// gcc's heapsort sift-down. Values defined inside are used after the cycle.
const char *IRREDUCIBLE = R"(
	define i32 @f(i1 %c0, i1 %c1, i1 %c2, i1 %c3, i32 %x) {
	entry:
		br i1 %c0, label %a, label %b
	a:
		%pa = phi i32 [ %x, %entry ], [ %vm, %m ]
		%va = add i32 %pa, 1
		br i1 %c1, label %m, label %b
	b:
		%pb = phi i32 [ %x, %entry ], [ %va, %a ], [ %vm, %b2 ]
		%vb = mul i32 %pb, 3
		br i1 %c2, label %m, label %exit
	m:
		%pm = phi i32 [ %va, %a ], [ %vb, %b ]
		%vm = sub i32 %pm, 2
		br i1 %c3, label %a, label %b2
	b2:
		br label %b
	exit:
		ret i32 %vb
	}
)";

std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &ctx, const char *ir) {
	llvm::SMDiagnostic err;
	auto m = llvm::parseAssemblyString(ir, err, ctx);
	EXPECT_TRUE(m) << err.getMessage().str();
	return m;
}

unsigned multiEntryCycles(llvm::Function &f) {
	unsigned n = 0;
	for (auto it = llvm::scc_begin(&f); !it.isAtEnd(); ++it) {
		if (it->size() < 2) {
			continue;
		}
		std::set<llvm::BasicBlock *> scc(it->begin(), it->end());
		unsigned entries = 0;
		for (auto bb : *it) {
			for (auto pred : llvm::predecessors(bb)) {
				if (!scc.count(pred)) {
					++entries;
					break;
				}
			}
		}
		n += entries > 1;
	}
	return n;
}

} // anonymous namespace

TEST(IrreducibleCycleSplitterTests, MultiEntryCycleBecomesNaturalLoop) {
	llvm::LLVMContext ctx;
	auto m = parse(ctx, IRREDUCIBLE);
	auto &f = *m->getFunction("f");
	ASSERT_EQ(1u, multiEntryCycles(f));

	EXPECT_GT(splitIrreducibleCycles(f, 100, 100), 0u);

	EXPECT_FALSE(llvm::verifyFunction(f, &llvm::errs()));
	EXPECT_EQ(0u, multiEntryCycles(f));
	llvm::DominatorTree dt(f);
	llvm::LoopInfo li(dt);
	EXPECT_FALSE(li.empty());
}

TEST(IrreducibleCycleSplitterTests, NothingIsCopiedOverBudget) {
	llvm::LLVMContext ctx;
	auto m = parse(ctx, IRREDUCIBLE);
	auto &f = *m->getFunction("f");
	const auto before = f.getInstructionCount();

	EXPECT_EQ(0u, splitIrreducibleCycles(f, 1, 100));
	EXPECT_EQ(0u, splitIrreducibleCycles(f, 100, 1));

	EXPECT_EQ(before, f.getInstructionCount());
	EXPECT_EQ(1u, multiEntryCycles(f));
}

TEST(IrreducibleCycleSplitterTests, ReducibleLoopIsLeftAlone) {
	llvm::LLVMContext ctx;
	auto m = parse(ctx, R"(
		define void @f(i1 %c) {
		entry:
			br label %h
		h:
			br i1 %c, label %h, label %exit
		exit:
			ret void
		}
	)");
	auto &f = *m->getFunction("f");

	EXPECT_EQ(0u, splitIrreducibleCycles(f, 100, 100));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
