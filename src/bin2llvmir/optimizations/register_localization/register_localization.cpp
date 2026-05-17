/**
* @file src/bin2llvmir/optimizations/register_localization/register_localization.cpp
* @brief Make all registers local.
* @copyright (c) 2019 Odin Loch Trading as Imortek
*/

#include <cassert>
#include <vector>
#include <cstdlib>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/raw_ostream.h>

#include "retdec/bin2llvmir/optimizations/register_localization/register_localization.h"
#include "retdec/bin2llvmir/providers/names.h"
#include "retdec/bin2llvmir/utils/debug.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace retdec::utils;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {

namespace {

bool regLocTraceEnabled()
{
	const char* env = std::getenv("RETDEC_REGLOC_TRACE");
	return env && *env && std::string(env) != "0";
}

void traceRegLoc(const std::string& msg)
{
	if (!regLocTraceEnabled())
	{
		return;
	}
	llvm::errs() << "[regloc] " << msg << "\n";
	llvm::errs().flush();
}

}

char RegisterLocalization::ID = 0;

static RegisterPass<RegisterLocalization> X(
		"retdec-register-localization",
		"Make all registers local",
		false, // Only looks at CFG
		false // Analysis Pass
);

RegisterLocalization::RegisterLocalization() :
		ModulePass(ID)
{

}

bool RegisterLocalization::runOnModule(Module& M)
{
	_module = &M;
	_abi = AbiProvider::getAbi(_module);
	_config = ConfigProvider::getConfig(_module);
	return run();
}

bool RegisterLocalization::runOnModuleCustom(llvm::Module& M, Abi* a, Config* c)
{
	_module = &M;
	_abi = a;
	_config = c;
	return run();
}

/**
 * @return @c True if module @a _module was modified in any way,
 *         @c false otherwise.
 */
bool RegisterLocalization::run()
{
	bool changed = false;
	if (_abi != nullptr && _config != nullptr)
	{
		const auto& regs = _abi->getRegisters();
		for (GlobalVariable* reg : regs)
		{
			std::map<Function*, AllocaInst*> fnc2alloca;

			for (auto uIt = reg->user_begin(); uIt != reg->user_end(); )
			{
				User* user = *uIt;
				++uIt;

				if (auto* insn = dyn_cast<Instruction>(user))
				{
					changed = localize(reg, fnc2alloca, insn);
				}
				else if (auto* expr = dyn_cast<ConstantExpr>(user))
				{
					for (auto euIt = expr->user_begin(); euIt != expr->user_end(); )
					{
						User* euser = *euIt;
						++euIt;

						if (auto* insn = dyn_cast<Instruction>(euser))
						{
							auto* einsn = expr->getAsInstruction();
							einsn->insertBefore(insn);

							if (localize(reg, fnc2alloca, einsn))
							{
								insn->replaceUsesOfWith(expr, einsn);
								changed = true;
							}
						}
					}
				}
			}
		}
	}

	// Ensure stack variables reconstructed by earlier passes dominate all uses.
	// Some transformations may leave stack_var_* allocas in non-entry blocks
	// or after first uses in the entry block.
	for (Function& f : *_module)
	{
		if (f.empty())
		{
			continue;
		}

		auto insertIt = f.getEntryBlock().getFirstInsertionPt();
		if (insertIt == f.getEntryBlock().end())
		{
			continue;
		}
		Instruction* insertPt = &*insertIt;
		std::vector<AllocaInst*> toMove;
		unsigned nonEntryAllocas = 0;

		// Collect all constant-size allocas in the function, regardless of
		// whether they are currently in the entry block.  isStaticAlloca()
		// returns false for non-entry allocas, so we must NOT use it here.
		for (auto& bb : f)
		{
			for (auto& instr : bb)
			{
				auto* a = dyn_cast<AllocaInst>(&instr);
				if (a == nullptr)
				{
					continue;
				}
				if (!isa<ConstantInt>(a->getArraySize()))
				{
					continue;
				}
				if (a->hasName() && a->getName().startswith("stack_var_"))
				{
					traceRegLoc(
						"stack-var "
						+ a->getName().str()
						+ " in "
						+ f.getName().str()
						+ ", block="
						+ a->getParent()->getName().str()
						+ ", in-entry="
						+ (a->getParent() == &f.getEntryBlock() ? std::string("1") : std::string("0")));
				}
				toMove.push_back(a);
				if (a->getParent() != &f.getEntryBlock())
				{
					++nonEntryAllocas;
				}
			}
		}

		for (auto* a : toMove)
		{
			if (a != insertPt)
			{
				a->moveBefore(insertPt);
				changed = true;
			}
		}
		// If any alloca is still used earlier in the same block, move it before
		// the earliest such use to satisfy dominance in malformed lowered IR.
		for (auto* a : toMove)
		{
			Instruction* earliest = nullptr;
			auto isBefore = [](Instruction* lhs, Instruction* rhs) {
				if (lhs == nullptr || rhs == nullptr || lhs->getParent() != rhs->getParent())
				{
					return false;
				}
				for (Instruction& i : *lhs->getParent())
				{
					if (&i == lhs)
					{
						return true;
					}
					if (&i == rhs)
					{
						return false;
					}
				}
				return false;
			};
			for (User* u : a->users())
			{
				auto* ui = dyn_cast<Instruction>(u);
				if (ui == nullptr || ui->getParent() != a->getParent())
				{
					continue;
				}
				if (isBefore(ui, a))
				{
					if (earliest == nullptr || isBefore(ui, earliest))
					{
						earliest = ui;
					}
				}
			}
			if (earliest != nullptr)
			{
				traceRegLoc(
					"reorder alloca before in-block use: "
					+ a->getName().str()
					+ " in "
					+ f.getName().str());
				a->moveBefore(earliest);
				changed = true;
			}
		}
		if (!toMove.empty())
		{
			traceRegLoc(
				"static allocas seen in "
				+ f.getName().str()
				+ ": "
				+ std::to_string(toMove.size())
				+ ", non-entry: "
				+ std::to_string(nonEntryAllocas));
		}
	}

	return changed;
}

llvm::AllocaInst* RegisterLocalization::getLocalized(
		llvm::GlobalVariable* reg,
		llvm::Function* fnc,
		std::map<llvm::Function*, llvm::AllocaInst*>& fnc2alloca)
{
		auto fIt = fnc2alloca.find(fnc);
		if (fIt != fnc2alloca.end())
		{
			return fIt->second;
		}
		else if (!fnc->empty() && !fnc->front().empty())
		{
			auto* a = new AllocaInst(
					reg->getValueType(),
					reg->getAddressSpace(),
					nullptr,
					reg->getName(),
					&fnc->front().front());
			fnc2alloca.emplace(fnc, a);
			return a;
		}
		else
		{
			// Should not really happen.
			return nullptr;
		}
}

bool RegisterLocalization::localize(
		llvm::GlobalVariable* reg,
		std::map<llvm::Function*, llvm::AllocaInst*>& fnc2alloca,
		llvm::Instruction* insn)
{
	AllocaInst* localized = getLocalized(
			reg,
			insn->getFunction(),
			fnc2alloca);

	if (localized == nullptr)
	{
		return false;
	}

	insn->replaceUsesOfWith(reg, localized);
	return true;
}

} // namespace bin2llvmir
} // namespace retdec
