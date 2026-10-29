/**
* @file src/bin2llvmir/optimizations/stack/stack.cpp
* @brief Reconstruct stack.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <regex>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/optimizations/stack/stack.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#define debug_enabled false
#include "retdec/bin2llvmir/utils/llvm.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char StackAnalysis::ID = 0;

static RegisterPass<StackAnalysis> X(
		"retdec-stack",
		"Stack optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

StackAnalysis::StackAnalysis() :
		ModulePass(ID)
{

}

bool StackAnalysis::runOnModule(llvm::Module& m)
{
	_module = &m;
	_config = ConfigProvider::getConfig(_module);
	_abi = AbiProvider::getAbi(_module);
	_dbgf = DebugFormatProvider::getDebugFormat(_module);
	return run();
}

bool StackAnalysis::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		Abi* abi,
		DebugFormat* dbgf)
{
	_module = &m;
	_config = c;
	_abi = abi;
	_dbgf = dbgf;
	return run();
}

bool StackAnalysis::run()
{
	if (_config == nullptr)
	{
		return false;
	}

	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*_module, _abi);

	for (auto& f : *_module)
	{
		std::map<Value*, Value*> val2val;
		for (inst_iterator I = inst_begin(f), E = inst_end(f); I != E;)
		{
			Instruction& i = *I;
			++I;

			if (StoreInst *store = dyn_cast<StoreInst>(&i))
			{
				if (AsmInstruction::isLlvmToAsmInstruction(store))
				{
					continue;
				}

				handleInstruction(
						RDA,
						store,
						store->getValueOperand(),
						store->getValueOperand()->getType(),
						val2val);

				if (isa<GlobalVariable>(store->getPointerOperand()))
				{
					continue;
				}

				handleInstruction(
						RDA,
						store,
						store->getPointerOperand(),
						store->getValueOperand()->getType(),
						val2val);
			}
			else if (LoadInst* load = dyn_cast<LoadInst>(&i))
			{
				if (isa<GlobalVariable>(load->getPointerOperand()))
				{
					continue;
				}

				handleInstruction(
						RDA,
						load,
						load->getPointerOperand(),
						load->getType(),
						val2val);
			}
		}
	}

	IrModifier::eraseUnusedInstructionsRecursive(_toRemove);

	for (auto& f : *_module)
	{
		mergeWalkedLocals(f);
	}

	return false;
}


namespace {

std::optional<int> stackOffsetOf(Config* config, AllocaInst* a)
{
	if (auto o = config->getStackVariableOffset(a)) return o;
	static const std::regex re("^stack_var_(-?[0-9]+)(\\.typed[0-9]*)?$");
	std::smatch m;
	std::string n = a->getName().str();
	if (std::regex_match(n, m, re)) return std::stoi(m[1].str());
	return std::nullopt;
}


} // anonymous namespace

/**
 * Local slots one pointer walks across are one array, not separate variables.
 *
 * `lea 0x8(%rsp),%rax ... addsd (%rax),%xmm1; add $0x10,%rax; cmp %rax,%rcx`
 * reads `double a[5]` at rsp..rsp+0x28 through a pointer, but every constant
 * offset becomes its own alloca.  LLVM then sees the `movaps` that wrote
 * a[0..1] go to an alloca nothing loads from and deletes it -- the array's
 * initial values vanished from float_mean_variance -O2.  When addresses of
 * negative-offset slots are taken and a slot between them is only written, put
 * every local slot between the lowest and highest such address into one
 * frame object, so the stores and the walked loads alias again.
 */
void StackAnalysis::mergeWalkedLocals(llvm::Function& f)
{
	if (f.isDeclaration()) return;
	const auto& dl = _module->getDataLayout();
	std::vector<std::pair<AllocaInst*, int>> slots;
	std::optional<int> lo, hi;
	for (auto& i : f.getEntryBlock())
	{
		auto* a = dyn_cast<AllocaInst>(&i);
		if (a == nullptr || !a->isStaticAlloca()) continue;
		auto o = stackOffsetOf(_config, a);
		if (!o.has_value() || *o >= 0) continue;
		slots.push_back({a, *o});
		bool walked = false;
		for (auto* u : a->users())
			if (isa<PtrToIntInst>(u)) walked = true;
		if (!walked) continue;
		lo = lo ? std::min(*lo, *o) : *o;
		hi = hi ? std::max(*hi, *o) : *o;
	}
	if (!lo || *lo == *hi) return;

	std::vector<std::pair<AllocaInst*, int>> members;
	int end = *hi;
	for (auto& [a, o] : slots)
	{
		if (o < *lo || o > *hi) continue;
		members.push_back({a, o});
		end = std::max<int>(end, o + int(dl.getTypeAllocSize(a->getAllocatedType())));
	}
	if (members.size() < 2) return;
	// Only when some slot in the range is written but never read directly:
	// that write is reachable only through the taken addresses.
	bool writeOnly = false;
	for (auto& [a, o] : members)
	{
		bool st = false, other = false;
		for (auto* u : a->users())
		{
			if (auto* si = dyn_cast<StoreInst>(u); si && si->getPointerOperand() == a) st = true;
			else other = true;
		}
		if (st && !other) writeOnly = true;
	}
	if (!writeOnly) return;

	IRBuilder<> irb(&*f.getEntryBlock().getFirstInsertionPt());
	auto* frameTy = ArrayType::get(irb.getInt8Ty(), end - *lo);
	auto* frame = irb.CreateAlloca(frameTy, nullptr, "stack_frame_" + std::to_string(-*lo));
	frame->setAlignment(Align(16));
	for (auto& [a, o] : members)
	{
		irb.SetInsertPoint(a);
		auto* gep = irb.CreateConstInBoundsGEP2_32(frameTy, frame, 0, o - *lo, a->getName());
		a->replaceAllUsesWith(gep);
		a->eraseFromParent();
	}
}

void StackAnalysis::handleInstruction(
		ReachingDefinitionsAnalysis& RDA,
		llvm::Instruction* inst,
		llvm::Value* val,
		llvm::Type* type,
		std::map<llvm::Value*, llvm::Value*>& val2val)
{
	LOG << llvmObjToString(inst) << std::endl;

	auto root = SymbolicTree::PrecomputedRdaWithValueMap(RDA, val, &val2val);
	LOG << root << std::endl;

	if (!root.isVal2ValMapUsed())
	{
		bool stackPtr = false;
		for (SymbolicTree* n : root.getPostOrder())
		{
			if (_abi->isStackPointerRegister(n->value))
			{
				stackPtr = true;
				break;
			}
		}
		if (!stackPtr)
		{
			LOG << "===> no SP" << std::endl;
			return;
		}
	}

	auto* debugSv = getDebugStackVariable(inst->getFunction(), root);
	auto* configSv = getConfigStackVariable(inst->getFunction(), root);

	root.simplifyNode();
	LOG << root << std::endl;

	if (debugSv == nullptr)
	{
		debugSv = getDebugStackVariable(inst->getFunction(), root);
	}

	if (configSv == nullptr)
	{
		configSv = getConfigStackVariable(inst->getFunction(), root);
	}

	auto* ci = dyn_cast_or_null<ConstantInt>(root.value);
	if (ci == nullptr)
	{
		return;
	}

	if (auto* s = dyn_cast<StoreInst>(inst))
	{
		if (s->getValueOperand() == val)
		{
			val2val[inst] = ci;
		}
	}

	LOG << "===> " << llvmObjToString(ci) << std::endl;
	LOG << "===> " << ci->getSExtValue() << std::endl;

	std::string name = "";
	Type* t = type;
	auto isCompatiblePreferredType = [this](Type* preferred, Type* observed) -> bool
	{
		if (preferred == nullptr || observed == nullptr)
		{
			return false;
		}
		if (preferred == observed)
		{
			return true;
		}
		// Avoid forcing debug/config pointer types onto scalar stack slots
		// (and vice versa). That can produce malformed IR for reused offsets.
		if (preferred->isPointerTy() != observed->isPointerTy())
		{
			return false;
		}
		if (preferred->isPointerTy())
		{
			return true;
		}
		if (!preferred->isSized() || !observed->isSized())
		{
			return false;
		}
		return _module->getDataLayout().getTypeStoreSize(preferred)
				== _module->getDataLayout().getTypeStoreSize(observed);
	};

	if (debugSv)
	{
		name = debugSv->getName();
		auto* preferred = llvm_utils::stringToLlvmTypeDefault(_module, debugSv->type.getLlvmIr());
		if (isCompatiblePreferredType(preferred, type))
		{
			t = preferred;
		}
	}
	else if (configSv)
	{
		name = configSv->getName();
		auto* preferred = llvm_utils::stringToLlvmTypeDefault(_module, configSv->type.getLlvmIr());
		if (isCompatiblePreferredType(preferred, type))
		{
			t = preferred;
		}
	}

	std::string realName;
	if (debugSv)
	{
		realName = debugSv->getName();
	}
	else if (configSv)
	{
		realName = configSv->getName();
	}

	IrModifier irModif(_module, _config);
	auto p = irModif.getStackVariable(
			inst->getFunction(),
			ci->getSExtValue(),
			t,
			name,
			realName,
			debugSv || configSv);

	AllocaInst* a = p.first;
	if (a != nullptr && t != nullptr && a->getAllocatedType() != t)
	{
		// Same stack offset can be observed with incompatible types in complex
		// lowered IR. Keep this access typed-correct by using a dedicated alloca
		// instead of forcing incompatible loads/stores on the shared slot.
		// Accesses at the same type share one alloca: giving each access its
		// own made a store and the later loads of the same slot unrelated
		// variables, so the store was removed as dead.
		auto ip = inst->getFunction()->getEntryBlock().getFirstInsertionPt();
		auto& view = _typedViews[{a, t}];
		if (view)
		{
			a = view;
		}
		else if (ip != inst->getFunction()->getEntryBlock().end())
		{
			std::string typedName = a->hasName()
					? (a->getName().str() + ".typed")
					: "stack_var_typed";
			a = new AllocaInst(t, Abi::DEFAULT_ADDR_SPACE, typedName, &*ip);
			view = a;
		}
	}

	LOG << "===> " << llvmObjToString(a) << std::endl;
	LOG << "===> " << llvmObjToString(inst) << std::endl;
	LOG << std::endl;

	auto* s = dyn_cast<StoreInst>(inst);
	auto* l = dyn_cast<LoadInst>(inst);
	if (s && s->getPointerOperand() == val)
	{
		auto* conv = IrModifier::convertValueToType(
				s->getValueOperand(),
				llvm_utils::pointeeType(a),
				inst);
		auto* ns = llvm_utils::createStoreInst(conv, a, inst);
		_toRemove.insert(s);
	}
	else if (l && l->getPointerOperand() == val)
	{
		auto* nl = llvm_utils::createLoadInst(a, "", l);
		auto* conv = IrModifier::convertValueToType(nl, l->getType(), l);
		l->replaceAllUsesWith(conv);
		_toRemove.insert(l);
	}
	else
	{
		auto* conv = IrModifier::convertValueToType(a, val->getType(), inst);
		_toRemove.insert(val);
		inst->replaceUsesOfWith(val, conv);
	}
}

std::optional<int> StackAnalysis::getBaseOffset(SymbolicTree& root)
{
	std::optional<int> baseOffset;
	if (auto* ci = dyn_cast_or_null<ConstantInt>(root.value))
	{
		baseOffset = ci->getSExtValue();
	}
	else
	{
		for (SymbolicTree* n : root.getLevelOrder())
		{
			if (isa<AddOperator>(n->value)
					&& n->ops.size() == 2
					&& isa<LoadInst>(n->ops[0].value)
					&& isa<ConstantInt>(n->ops[1].value))
			{
				auto* l = cast<LoadInst>(n->ops[0].value);
				auto* ci = cast<ConstantInt>(n->ops[1].value);
				if (_abi->isRegister(l->getPointerOperand()))
				{
					baseOffset = ci->getSExtValue();
				}
				break;
			}
		}
	}

	return baseOffset;
}

/**
 * Find a value that is being added to the stack pointer register in \p root.
 * Find a debug variable with offset equal to this value.
 */
const retdec::common::Object* StackAnalysis::getDebugStackVariable(
		llvm::Function* fnc,
		SymbolicTree& root)
{
	auto baseOffset = getBaseOffset(root);
	if (!baseOffset.has_value())
	{
		return nullptr;
	}

	if (_dbgf == nullptr)
	{
		return nullptr;
	}

	auto* debugFnc = _dbgf->getFunction(_config->getFunctionAddress(fnc));
	if (debugFnc == nullptr)
	{
		return nullptr;
	}

	for (auto& var : debugFnc->locals)
	{
		if (!var.getStorage().isStack())
		{
			continue;
		}
		if (var.getStorage().getStackOffset() == baseOffset)
		{
			return &var;
		}
	}

	return nullptr;
}

const retdec::common::Object* StackAnalysis::getConfigStackVariable(
		llvm::Function* fnc,
		SymbolicTree& root)
{
	auto baseOffset = getBaseOffset(root);
	if (!baseOffset.has_value())
	{
		return nullptr;
	}

	auto cfn = _config->getConfigFunction(fnc);
	if (cfn && _config->getLlvmStackVariable(fnc, baseOffset.value()) == nullptr)
	{
		for (auto& var: cfn->locals)
		{
			if (var.getStorage().getStackOffset() == baseOffset)
			{
				return &var;
			}
		}
	}

	return nullptr;
}

} // namespace bin2llvmir
} // namespace retdec
