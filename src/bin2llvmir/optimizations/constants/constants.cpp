/**
* @file src/bin2llvmir/optimizations/constants/constants.cpp
* @brief Composite type reconstruction analysis.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <cassert>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

#include "retdec/utils/string.h"
#include "retdec/utils/time.h"
#include "retdec/bin2llvmir/analyses/symbolic_tree.h"
#include "retdec/bin2llvmir/optimizations/constants/constants.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
const bool debug_enabled = false;
#include "retdec/bin2llvmir/utils/llvm.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace retdec::utils;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char ConstantsAnalysis::ID = 0;

static RegisterPass<ConstantsAnalysis> X(
		"retdec-constants",
		"Constants optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

ConstantsAnalysis::ConstantsAnalysis() :
		ModulePass(ID)
{

}

bool ConstantsAnalysis::runOnModule(llvm::Module& m)
{
	_module = &m;
	_config = ConfigProvider::getConfig(_module);
	_abi = AbiProvider::getAbi(_module);
	_image = FileImageProvider::getFileImage(_module);
	_dbgf = DebugFormatProvider::getDebugFormat(_module);
	return run();
}

bool ConstantsAnalysis::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		Abi* a,
		FileImage* i,
		DebugFormat* d)
{
	_module = &m;
	_config = c;
	_abi = a;
	_image = i;
	_dbgf = d;
	return run();
}

bool ConstantsAnalysis::run()
{
	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*_module, _abi);

	for (Function& f : *_module)
	for (inst_iterator I = inst_begin(&f), E = inst_end(&f); I != E;)
	{
		Instruction& i = *I;
		++I;

		if (StoreInst *store = dyn_cast<StoreInst>(&i))
		{
			if (AsmInstruction::isLlvmToAsmInstruction(store))
			{
				continue;
			}

			checkForGlobalInInstruction(RDA, store, store->getValueOperand(), true);

			if (isa<GlobalVariable>(store->getPointerOperand()))
			{
				continue;
			}

			checkForGlobalInInstruction(RDA, store, store->getPointerOperand());
		}
		else if (auto* load = dyn_cast<LoadInst>(&i))
		{
			if (isa<GlobalVariable>(load->getPointerOperand()))
			{
				continue;
			}

			checkForGlobalInInstruction(RDA, load, load->getPointerOperand());
		}
	}

	IrModifier::eraseUnusedInstructionsRecursive(_toRemove);

	widenNarrowConstantLoads();
	foldUnwrittenMxcsr();

	return false;
}

/**
 * The x86 lifter rounds MXCSR-controlled conversions by MXCSR.RC read from
 * the @c mxcsr global. A program with no LDMXCSR never changes it, so its
 * power-on value holds everywhere: fold the loads and the rounding selects
 * collapse back to round-to-nearest-even.
 */
void ConstantsAnalysis::foldUnwrittenMxcsr()
{
	auto* gv = _module->getGlobalVariable("mxcsr");
	if (gv == nullptr || !gv->hasInitializer()) return;
	std::vector<LoadInst*> loads;
	for (auto* u : gv->users())
	{
		if (auto* l = dyn_cast<LoadInst>(u)) loads.push_back(l);
		else return; // a store or an escaped address: not provably unwritten
	}
	for (auto* l : loads)
	{
		l->replaceAllUsesWith(gv->getInitializer());
		l->eraseFromParent();
	}
}

/**
 * `movapd 0x2010(%rip),%xmm0` reads 16 bytes, but the global made for 0x2010
 * may be typed by an earlier `movsd` of the same address as an 8-byte i64.
 * LLVM then folds the i128 load of that 8-byte constant with its upper half
 * as zero: {2.0, 4.0} became {2.0, 0.0}.  Read such wide loads of read-only
 * data straight from the image instead.
 */
void ConstantsAnalysis::widenNarrowConstantLoads()
{
	auto& dl = _module->getDataLayout();
	auto* img = _image->getImage();
	for (Function& f : *_module)
	for (inst_iterator I = inst_begin(&f), E = inst_end(&f); I != E;)
	{
		auto* load = dyn_cast<LoadInst>(&*I);
		++I;
		if (load == nullptr || !load->getType()->isIntegerTy()) continue;
		auto* gv = dyn_cast<GlobalVariable>(load->getPointerOperand());
		if (gv == nullptr || !gv->isConstant() || !gv->hasInitializer()) continue;
		uint64_t want = dl.getTypeStoreSize(load->getType());
		if (want <= dl.getTypeStoreSize(gv->getValueType())) continue;
		auto addr = _config->getGlobalAddress(gv);
		if (addr.isUndefined()) continue;
		std::vector<std::uint8_t> bytes;
		if (!img->getXBytes(addr, want, bytes) || bytes.size() < want) continue;
		unsigned bits = load->getType()->getIntegerBitWidth();
		APInt v(bits, 0);
		bool le = dl.isLittleEndian();
		for (uint64_t k = 0; k < want; ++k)
		{
			uint64_t b = bytes[le ? want - 1 - k : k];
			v = v.shl(8) | APInt(bits, b);
		}
		load->replaceAllUsesWith(ConstantInt::get(load->getType(), v));
		load->eraseFromParent();
	}
}

void ConstantsAnalysis::checkForGlobalInInstruction(
		ReachingDefinitionsAnalysis& RDA,
		Instruction* inst,
		Value* val,
		bool storeValue)
{
	LOG << llvmObjToString(inst) << std::endl;

	auto root = SymbolicTree::PrecomputedRda(RDA, val);
	root.simplifyNode();

	LOG << root << std::endl;

	auto* max = root.getMaxIntValue();
	auto* maxC = max ? dyn_cast_or_null<ConstantInt>(max->value) : nullptr;
	Instruction* userI = max ? dyn_cast_or_null<Instruction>(max->user) : nullptr;

	if (max && maxC && maxC->getValue().getActiveBits() <= 64 && maxC->getZExtValue() != 0)
	if (userI || max == &root)
	if (_image->getImage()->hasDataOnAddress(maxC->getZExtValue()))
	{
		IrModifier irm(_module, _config);
		// Stage 20: Width-seeded — use load/store width for global pointee type
		Type* widthSeed = nullptr;
		if (max == &root && isa<LoadInst>(inst))
			widthSeed = inst->getType();  // load result type
		else if (isa<StoreInst>(inst))
			widthSeed = val->getType();   // stored value type
		auto* ngv = irm.getGlobalVariable(
				_image,
				_dbgf,
				maxC->getZExtValue(),
				storeValue,
				std::string(),
				widthSeed);

		if (ngv)
		{
			if (max == &root)
			{
				auto* conv = IrModifier::convertConstantToType(ngv, val->getType());
				_toRemove.insert(val);
				inst->replaceUsesOfWith(val, conv);
				return;
			}
			else if (userI)
			{
				auto* conv = IrModifier::convertConstantToType(ngv, maxC->getType());
				userI->replaceUsesOfWith(maxC, conv);
				return;
			}
		}
	}

	auto* gv = dyn_cast<GlobalVariable>(root.value);
	if (isa<LoadInst>(inst) && gv && root.ops.size() <= 1)
	{
		auto* conv = IrModifier::convertConstantToType(gv, val->getType());
		_toRemove.insert(val);
		inst->replaceUsesOfWith(val, conv);
		return;
	}
}

} // namespace bin2llvmir
} // namespace retdec
