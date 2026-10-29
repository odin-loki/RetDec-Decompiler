/**
* @file src/bin2llvmir/optimizations/stack/stack.cpp
* @brief Reconstruct stack.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <algorithm>
#include <set>
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
		mergeStoredOverLocals(f);
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


/// Follows @a v through casts and through register round-trips within one
/// basic block (`store X, @reg ... load @reg`) to the value it came from.
Value* resolveLocally(Value* v, int depth = 0)
{
	for (; v && depth < 16; ++depth)
	{
		if (auto* c = dyn_cast<CastInst>(v))
		{
			v = c->getOperand(0);
			continue;
		}
		auto* l = dyn_cast<LoadInst>(v);
		if (l == nullptr) return v;
		auto* ptr = l->getPointerOperand();
		if (!isa<GlobalVariable>(ptr) && !isa<AllocaInst>(ptr)) return v;
		StoreInst* def = nullptr;
		for (auto it = l->getReverseIterator(), e = l->getParent()->rend(); it != e; ++it)
		{
			if (auto* s = dyn_cast<StoreInst>(&*it); s && s->getPointerOperand() == ptr)
			{
				def = s;
				break;
			}
			if (auto* call = dyn_cast<CallInst>(&*it))
			{
				auto* fn = call->getCalledFunction();
				if (fn == nullptr || !(fn->isIntrinsic() || fn->getName().starts_with("__asm_"))) return v;
			}
		}
		if (def == nullptr) return v;
		v = def->getValueOperand();
	}
	return v;
}

/// Byte width of the element a `__asm_rep_stos?_memset` stores, or 0.
unsigned repStosWidth(const CallInst* call)
{
	auto* fn = call ? call->getCalledFunction() : nullptr;
	if (fn == nullptr || call->arg_size() != 3) return 0;
	auto n = fn->getName();
	if (n == "__asm_rep_stosb_memset") return 1;
	if (n == "__asm_rep_stosw_memset") return 2;
	if (n == "__asm_rep_stosd_memset") return 4;
	if (n == "__asm_rep_stosq_memset") return 8;
	return 0;
}

/// Do two resolved values read the same thing (same value, or two loads of
/// one register with nothing writing it in between)?
bool sameValue(Value* a, Value* b)
{
	if (a == b) return true;
	auto* la = dyn_cast_or_null<LoadInst>(a);
	auto* lb = dyn_cast_or_null<LoadInst>(b);
	if (!la || !lb || la->getParent() != lb->getParent()
			|| la->getPointerOperand() != lb->getPointerOperand())
		return false;
	if (lb->comesBefore(la)) std::swap(la, lb);
	for (auto* i = la->getNextNode(); i && i != lb; i = i->getNextNode())
	{
		if (auto* s = dyn_cast<StoreInst>(i); s && s->getPointerOperand() == la->getPointerOperand())
			return false;
		if (auto* c = dyn_cast<CallInst>(i))
		{
			auto* fn = c->getCalledFunction();
			if (fn == nullptr || !(fn->isIntrinsic() || fn->getName().starts_with("__asm_"))) return false;
		}
	}
	return true;
}

/// Constant value of a register operand, folding `xor r,r` zeroing idioms.
ConstantInt* resolveConstInt(Value* v, int depth = 0)
{
	v = resolveLocally(v);
	if (auto* c = dyn_cast_or_null<ConstantInt>(v)) return c;
	auto* bo = dyn_cast_or_null<BinaryOperator>(v);
	if (bo == nullptr || depth > 8) return nullptr;
	auto op = bo->getOpcode();
	if ((op == Instruction::Xor || op == Instruction::Sub)
			&& sameValue(resolveLocally(bo->getOperand(0)), resolveLocally(bo->getOperand(1))))
		return ConstantInt::get(cast<IntegerType>(bo->getType()), 0);
	auto* a = resolveConstInt(bo->getOperand(0), depth + 1);
	auto* b = resolveConstInt(bo->getOperand(1), depth + 1);
	if (a == nullptr || b == nullptr) return nullptr;
	unsigned bits = bo->getType()->getIntegerBitWidth();
	APInt x = a->getValue().zextOrTrunc(bits), y = b->getValue().zextOrTrunc(bits);
	switch (op)
	{
		case Instruction::And: return ConstantInt::get(bo->getContext(), x & y);
		case Instruction::Or: return ConstantInt::get(bo->getContext(), x | y);
		case Instruction::Xor: return ConstantInt::get(bo->getContext(), x ^ y);
		default: return nullptr;
	}
}

} // anonymous namespace

/**
 * A local array cleared with `rep stos` is one object, not the slots that
 * happen to be addressed by constant offsets inside it.
 *
 * `int dp[32] = {0}` at -O2 is `lea rdi,[rsp+8]; mov ecx,16; rep stosq`, and
 * `dp[cap]` is reached through another slot's address plus a computed
 * offset.  Kept as separate 8-byte allocas the clear overruns its slot and the
 * computed reads land on whatever the C compiler put next to it.  Every local
 * slot inside the cleared bytes joins one frame object, which is then emitted
 * as an array.
 */
void StackAnalysis::mergeStoredOverLocals(llvm::Function& f)
{
	if (f.isDeclaration()) return;
	const auto& dl = _module->getDataLayout();

	std::map<int, AllocaInst*> byOff;
	std::map<AllocaInst*, int> offOf;
	for (auto& i : f.getEntryBlock())
	{
		auto* a = dyn_cast<AllocaInst>(&i);
		if (a == nullptr || !a->isStaticAlloca()) continue;
		auto o = stackOffsetOf(_config, a);
		if (!o.has_value() || *o >= 0 || byOff.count(*o)) continue;
		byOff[*o] = a;
		offOf[a] = *o;
	}
	if (byOff.empty()) return;

	// [lo, hi) byte ranges the clears cover.
	std::vector<std::pair<int, int>> ranges;
	for (auto& bb : f)
	for (auto& i : bb)
	{
		auto* call = dyn_cast<CallInst>(&i);
		unsigned w = repStosWidth(call);
		if (w == 0) continue;
		auto* a = dyn_cast<AllocaInst>(resolveLocally(call->getArgOperand(0)));
		auto* cnt = dyn_cast<ConstantInt>(resolveLocally(call->getArgOperand(2)));
		if (a == nullptr || cnt == nullptr || !offOf.count(a)) continue;
		uint64_t bytes = cnt->getZExtValue() * w;
		if (bytes == 0 || bytes > (1u << 20)) continue;
		int lo = offOf[a];
		ranges.push_back({lo, lo + int(bytes)});
	}
	if (ranges.empty()) return;
	std::sort(ranges.begin(), ranges.end());

	// Merge overlapping ranges, growing each by the slots that start in it.
	std::vector<std::pair<int, int>> merged;
	for (auto r : ranges)
	{
		if (!merged.empty() && r.first < merged.back().second)
			merged.back().second = std::max(merged.back().second, r.second);
		else
			merged.push_back(r);
	}

	IRBuilder<> irb(&*f.getEntryBlock().getFirstInsertionPt());
	for (auto [lo, hi] : merged)
	{
		std::vector<std::pair<AllocaInst*, int>> members;
		for (auto it = byOff.lower_bound(lo); it != byOff.end() && it->first < hi; ++it)
		{
			members.push_back({it->second, it->first});
			hi = std::max<int>(hi, it->first
					+ int(dl.getTypeAllocSize(it->second->getAllocatedType())));
		}
		if (members.empty()) continue;
		if (members.size() == 1 && hi - lo <= int(dl.getTypeAllocSize(
				members[0].first->getAllocatedType())))
			continue;

		irb.SetInsertPoint(&*f.getEntryBlock().getFirstInsertionPt());
		auto* frameTy = ArrayType::get(irb.getInt8Ty(), hi - lo);
		auto* frame = irb.CreateAlloca(frameTy, nullptr,
				"stack_var_" + std::to_string(lo));
		frame->setAlignment(Align(16));
		for (auto& [a, o] : members)
		{
			irb.SetInsertPoint(a);
			Value* addr = o == lo
					? static_cast<Value*>(frame)
					: irb.CreateConstInBoundsGEP2_32(frameTy, frame, 0, o - lo);
			a->replaceAllUsesWith(addr);
			if (o == lo) frame->takeName(a);
			for (auto it = _typedViews.begin(); it != _typedViews.end();)
				it = (it->first.first == a || it->second == a) ? _typedViews.erase(it) : std::next(it);
			a->eraseFromParent();
		}
	}
}

/**
 * `rep stos` with a constant byte-uniform value and count over a local array
 * is `memset`: emit `llvm.memset` so the output reads `memset(a, 0, 128)`
 * instead of `__asm_rep_stosq_memset(&a, 0, 16)`.  The register updates the
 * lifter emits after the call are left alone.  Run from
 * retdec-stack-ptr-op-remove, after frame-pointer accesses are rebased onto
 * the array: before that the array looks unread and the memset is dead.
 */
bool StackAnalysis::lowerRepStosToMemset(llvm::Function& f)
{
	if (f.isDeclaration()) return false;
	const auto& dl = f.getParent()->getDataLayout();
	bool changed = false;
	std::vector<CallInst*> calls;
	for (auto& bb : f)
	for (auto& i : bb)
		if (repStosWidth(dyn_cast<CallInst>(&i))) calls.push_back(cast<CallInst>(&i));
	for (auto* call : calls)
	{
		unsigned w = repStosWidth(call);
		auto* cnt = resolveConstInt(call->getArgOperand(2));
		auto* val = resolveConstInt(call->getArgOperand(1));
		Value* ptr = resolveLocally(call->getArgOperand(0));
		if (cnt == nullptr || val == nullptr || ptr == nullptr
				|| !ptr->getType()->isPointerTy())
			continue;
		APInt off(dl.getIndexTypeSizeInBits(ptr->getType()), 0);
		auto* a = dyn_cast<AllocaInst>(
				ptr->stripAndAccumulateConstantOffsets(dl, off, true));
		if (a == nullptr || !a->isStaticAlloca() || off.isNegative()) continue;
		uint64_t bytes = cnt->getZExtValue() * w;
		uint64_t size = dl.getTypeAllocSize(a->getAllocatedType());
		if (bytes == 0 || off.getZExtValue() + bytes > size) continue;
		APInt v = val->getValue().zextOrTrunc(w * 8);
		uint8_t b = v.trunc(8).getZExtValue();
		if (v != APInt::getSplat(w * 8, APInt(8, b))) continue;
		IRBuilder<> irb(call);
		irb.CreateMemSet(ptr, irb.getInt8(b), bytes, MaybeAlign(1));
		call->eraseFromParent();
		changed = true;
	}
	return changed;
}

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
	if (!lo) return;

	// A slot above the highest taken address that is loaded but never stored
	// directly can only have been written through that address, typically by
	// a callee filling a struct (`addi a0,sp,20; jal init; lw a1,36(sp)`).
	// Grow the object over such slots; stop at the first slot written
	// directly, e.g. a saved register.
	auto onlyRead = [](AllocaInst* a) {
		bool ld = false;
		for (auto* u : a->users())
		{
			if (isa<LoadInst>(u)) ld = true;
			else return false;
		}
		return ld;
	};
	std::sort(slots.begin(), slots.end(),
			[](auto& x, auto& y) { return x.second < y.second; });
	bool readOnly = false;
	for (auto& [a, o] : slots)
	{
		if (o <= *hi) continue;
		if (!onlyRead(a)) break;
		hi = o;
		readOnly = true;
	}
	if (*lo == *hi) return;

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
	if (!writeOnly && !readOnly) return;

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

namespace {

bool isRealCall(const llvm::Instruction& i)
{
	auto* c = dyn_cast<CallInst>(&i);
	if (c == nullptr) return false;
	auto* f = c->getCalledFunction();
	return f == nullptr || !f->isIntrinsic();
}

/// Is there a call on the way from @a def to @a use?  Exact within one block;
/// across blocks only the tail of @a def's block and the head of @a use's are
/// scanned.
bool callBetween(llvm::Instruction* def, llvm::Instruction* use)
{
	if (def->getParent() == use->getParent() && def->comesBefore(use))
	{
		for (auto* i = def->getNextNode(); i && i != use; i = i->getNextNode())
			if (isRealCall(*i)) return true;
		return false;
	}
	for (auto* i = def->getNextNode(); i; i = i->getNextNode())
		if (isRealCall(*i)) return true;
	for (auto& i : *use->getParent())
	{
		if (&i == use) break;
		if (isRealCall(i)) return true;
	}
	return false;
}

} // anonymous namespace

bool StackAnalysis::returnRegisterCrossesCall(
		ReachingDefinitionsAnalysis& RDA,
		SymbolicTree& root)
{
	auto* cc = _abi ? _abi->getDefaultCallingConvention() : nullptr;
	if (cc == nullptr) return false;
	std::set<llvm::Value*> rets;
	for (auto r : cc->getReturnRegisters())
		if (auto* g = _abi->getRegister(r)) rets.insert(g);
	for (SymbolicTree* n : root.getPostOrder())
	{
		auto* l = dyn_cast<LoadInst>(n->value);
		if (l == nullptr || rets.count(l->getPointerOperand()) == 0) continue;
		for (auto* d : RDA.defsFromUse(l))
			if (callBetween(d->def, l)) return true;
	}
	return false;
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

	// Calls carry no arguments or results yet, so RDA sees the return
	// register flow straight through them: `mv a0,sp; jal sum; mv s0,a0`
	// made s0 a stack address instead of sum's result.
	if (returnRegisterCrossesCall(RDA, root))
	{
		LOG << "===> return register redefined by a call" << std::endl;
		return;
	}

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
	// Same stack offset observed at another type (an i64 `movabs` store of
	// a va_list's gp/fp offsets read back as i32): access the one slot at the
	// access's own type. A separate ".typed" alloca made the store and the
	// load unrelated variables, so the load read garbage (x64 -O1 vsum).
	// Only an access wider than the slot still gets a view of its own.
	Type* accessTy = nullptr;
	if (a != nullptr && t != nullptr && a->getAllocatedType() != t)
	{
		auto& dl = _module->getDataLayout();
		bool fits = t->isSized() && a->getAllocatedType()->isSized()
				&& dl.getTypeStoreSize(t)
					<= dl.getTypeStoreSize(a->getAllocatedType());
		if (fits)
		{
			accessTy = t;
		}
		else
		{
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
				accessTy ? accessTy : llvm_utils::pointeeType(a),
				inst);
		if (accessTy)
		{
			new StoreInst(conv, a, inst);
		}
		else
		{
			llvm_utils::createStoreInst(conv, a, inst);
		}
		_toRemove.insert(s);
	}
	else if (l && l->getPointerOperand() == val)
	{
		Value* nl = accessTy
				? static_cast<Value*>(new LoadInst(accessTy, a, "", l))
				: static_cast<Value*>(llvm_utils::createLoadInst(a, "", l));
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
