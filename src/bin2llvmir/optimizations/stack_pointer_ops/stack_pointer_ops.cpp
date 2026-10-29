/**
* @file src/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.cpp
* @brief Remove the remaining stack pointer operations.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <cassert>
#include <iomanip>
#include <functional>
#include <map>
#include <optional>
#include <regex>

#include <llvm/IR/IRBuilder.h>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

#include "retdec/bin2llvmir/utils/llvm.h"
#include "retdec/utils/string.h"
#include "retdec/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.h"
#include "retdec/bin2llvmir/utils/debug.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace retdec::utils;
using namespace llvm;

#define debug_enabled false

namespace retdec {
namespace bin2llvmir {

char StackPointerOpsRemove::ID = 0;

static RegisterPass<StackPointerOpsRemove> X(
		"retdec-stack-ptr-op-remove",
		"Stack pointer operations optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

StackPointerOpsRemove::StackPointerOpsRemove() :
		ModulePass(ID)
{

}

bool StackPointerOpsRemove::runOnModule(Module& m)
{
	_module = &m;
	_abi = AbiProvider::getAbi(_module);
	return run();
}

bool StackPointerOpsRemove::runOnModuleCustom(llvm::Module& m, Abi* a)
{
	_module = &m;
	_abi = a;
	return run();
}

/**
 * @return @c True if module @a _module was modified in any way,
 *         @c false otherwise.
 */
bool StackPointerOpsRemove::run()
{
	bool changed = false;
	changed |= removeStackPointerStores();
	changed |= removePreservationStores();
	changed |= rebaseIntoLocalArrays();

	return changed;
}

bool StackPointerOpsRemove::removeStackPointerStores()
{
	if (_abi == nullptr)
	{
		LOG << "[ABORT] ABI is not available\n";
		return false;
	}

	bool changed = false;
	for (auto& F : _module->getFunctionList())
	for (auto& B : F)
	{
		auto it = B.begin();
		while (it != B.end())
		{
			// We need to move to the next instruction before optimizing
			// (potentially removing) the current instruction. Otherwise,
			// the iterator would become invalid.
			//
			auto* inst = &(*it);
			++it;

			if (StoreInst* s = dyn_cast<StoreInst>(inst))
			{
				auto* reg = s->getPointerOperand();
				if (!_abi->isStackPointerRegister(reg))
				{
					continue;
				}

				LOG << "erase: " << llvmObjToString(inst) << std::endl;
				inst->eraseFromParent();
				changed = true;
			}
		}
	}

	return changed;
}

/**
 * Finds those allocas that are only used to store some value from ebp and then
 * this value is stored back to ebp.
 */
bool StackPointerOpsRemove::removePreservationStores()
{
	bool changed = false;

	// The sibling removeStackPointerStores() checks this; this one did not,
	// and run() calls both. AbiProvider::getAbi returns null when no ABI was
	// created, and the isRegister call below dereferences it.
	if (_abi == nullptr)
	{
		LOG << "[ABORT] ABI is not available\n";
		return false;
	}

	// Abi::isRegister(value, id) is a raw index into the register table with
	// no architecture check, and capstone's id spaces overlap: X86_REG_EBP is
	// 20, and on MIPS so is MIPS_REG_18, which is $s2. Erasing an $s2
	// spill/reload pair loses a callee-saved value the caller is entitled to
	// get back. On x86 the frame-pointer bookkeeping really is dead after
	// stack analysis, which is what this was written for.
	if (!_abi->isX86() && !_abi->isX64())
	{
		return false;
	}

	for (auto& f : _module->getFunctionList())
	{
		for (inst_iterator I = inst_begin(f), E = inst_end(f); I != E; ++I)
		{
			auto* a = dyn_cast<AllocaInst>(&*I);
			if (a == nullptr)
			{
				continue;
			}

			bool remove = true;
			Value* storedVal = nullptr;
			std::set<llvm::Instruction*> toRemove;
			for (auto* u : a->users())
			{
				if (auto* s = dyn_cast<StoreInst>(u))
				{
					auto* l = dyn_cast<LoadInst>(s->getValueOperand());
					if (l && storedVal == nullptr
							&& (_abi->isRegister(l->getPointerOperand(), X86_REG_EBP)
							|| _abi->isRegister(l->getPointerOperand(), X86_REG_RBP)))
					{
						storedVal = l->getPointerOperand();
						toRemove.insert(s);
					}
					else if (l && l->getPointerOperand() == storedVal)
					{
						toRemove.insert(s);
					}
					else
					{
						remove = false;
						break;
					}
				}
				else if (isa<LoadInst>(u) || isa<CastInst>(u))
				{
					for (auto* uu : u->users())
					{
						auto* s = dyn_cast<StoreInst>(uu);
						if (s && storedVal == nullptr
								&& (_abi->isRegister(s->getPointerOperand(), X86_REG_EBP)
								|| _abi->isRegister(s->getPointerOperand(), X86_REG_RBP)))
						{
							storedVal = s->getPointerOperand();
							toRemove.insert(s);
						}
						else if (s && s->getPointerOperand() == storedVal)
						{
							toRemove.insert(s);
						}
						else
						{
							remove = false;
							break;
						}
					}

					if (!remove)
					{
						break;
					}
				}
				else
				{
					remove = false;
					break;
				}
			}

			if (remove)
			{
				for (auto* i : toRemove)
				{
					i->eraseFromParent();
					changed = true;
				}
			}
		}
	}

	return changed;
}

namespace {

std::optional<int> localOffset(const AllocaInst* a)
{
	static const std::regex re("^stack_var_(-?[0-9]+)(\\.typed[0-9]*)?$");
	std::smatch m;
	std::string n = a->getName().str();
	if (std::regex_match(n, m, re)) return std::stoi(m[1].str());
	return std::nullopt;
}

} // anonymous namespace

/**
 * `rbp - 144 + 4*i` is `&dp[i]` when bp-152..bp-24 is the array `dp`, but the
 * frame pointer is the address of the saved-rbp slot, so the expression is
 * built on that slot.  Emitted as C, the slot and the array are unrelated
 * objects and the access misses the array.  Address arithmetic on one local
 * whose constant offset lands inside a local array is rebased on that array.
 */
bool StackPointerOpsRemove::rebaseIntoLocalArrays()
{
	bool changed = false;
	const auto& dl = _module->getDataLayout();
	for (auto& f : *_module)
	{
		if (f.isDeclaration()) continue;
		struct Slot { AllocaInst* a; int off; int size; };
		std::map<AllocaInst*, Slot> slots;
		std::vector<Slot> arrays;
		for (auto& i : f.getEntryBlock())
		{
			auto* a = dyn_cast<AllocaInst>(&i);
			if (a == nullptr || !a->isStaticAlloca()) continue;
			auto o = localOffset(a);
			if (!o) continue;
			Slot s{a, *o, int(dl.getTypeAllocSize(a->getAllocatedType()))};
			slots[a] = s;
			if (a->getAllocatedType()->isArrayTy()) arrays.push_back(s);
		}
		if (arrays.empty()) continue;

		auto arrayAt = [&](int off) -> const Slot* {
			for (auto& s : arrays)
				if (off >= s.off && off < s.off + s.size) return &s;
			return nullptr;
		};

		std::vector<BinaryOperator*> adds;
		for (auto& bb : f)
			for (auto& i : bb)
				if (auto* b = dyn_cast<BinaryOperator>(&i); b && b->getOpcode() == Instruction::Add)
					adds.push_back(b);

		for (auto* b : adds)
		{
			// Leaves of the add tree, two levels deep: one `ptrtoint A`, the
			// constants, and everything else.
			PtrToIntInst* p2i = nullptr;
			int64_t c = 0;
			bool haveC = false, bad = false;
			std::vector<Value*> rest;
			std::function<void(Value*, int)> walk = [&](Value* v, int depth) {
				auto* op = dyn_cast<BinaryOperator>(v);
				if (depth < 2 && op && op->getOpcode() == Instruction::Add)
				{
					walk(op->getOperand(0), depth + 1);
					walk(op->getOperand(1), depth + 1);
				}
				else if (auto* ci = dyn_cast<ConstantInt>(v))
				{
					c += ci->getSExtValue();
					haveC = true;
				}
				else if (auto* pi = dyn_cast<PtrToIntInst>(v); pi && isa<AllocaInst>(pi->getPointerOperand()))
				{
					if (p2i) bad = true;
					p2i = pi;
				}
				else rest.push_back(v);
			};
			walk(b, 0);
			if (bad || p2i == nullptr || !haveC) continue;
			auto* a = cast<AllocaInst>(p2i->getPointerOperand());
			if (!slots.count(a)) continue;
			auto& s = slots[a];
			int64_t target = int64_t(s.off) + c;
			if (target >= s.off && target < s.off + s.size) continue;
			auto* r = arrayAt(int(target));
			if (r == nullptr || r->a == a) continue;
			IRBuilder<> irb(b);
			Value* nv = irb.CreatePtrToInt(r->a, b->getType());
			for (auto* v : rest) nv = irb.CreateAdd(v, nv);
			if (target != r->off)
				nv = irb.CreateAdd(nv, ConstantInt::get(b->getType(), target - r->off));
			b->replaceAllUsesWith(nv);
			changed = true;
		}
	}
	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
