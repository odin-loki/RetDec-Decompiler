/**
* @file src/bin2llvmir/optimizations/constants/constants.cpp
* @brief Composite type reconstruction analysis.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <cassert>
#include <map>
#include <tuple>
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
	widenIndexedGlobals();
	foldUnwrittenMxcsr();

	return false;
}

/**
 * `g_arr[i & 15]` lifts to `load (add (shl i, 2), ptrtoint @g)`, but @g was
 * typed by the first access, e.g. as one i64.  Emitted as a scalar, the C
 * then indexes past an 8-byte object (garbage in the buildable output).  Give
 * a global that is indexed with a variable offset the size of its symbol, or
 * for an unnamed read-only table the bytes up to the next global or the end
 * of its section, as an array of its access type.
 */
void ConstantsAnalysis::widenIndexedGlobals()
{
	auto& dl = _module->getDataLayout();
	auto* img = _image->getImage();
	auto* ff = _image->getFileFormat();
	std::map<uint64_t, GlobalVariable*> byAddr;
	for (auto& gv : _module->globals())
	{
		auto a = _config->getGlobalAddress(&gv);
		if (a.isDefined()) byAddr[a] = &gv;
	}
	std::vector<std::tuple<GlobalVariable*, uint64_t, uint64_t>> work;
	for (auto& [addr, gv] : byAddr)
	{
		auto* et = gv->getValueType();
		if (!et->isIntegerTy() || !gv->hasInitializer()) continue;
		bool indexed = false;
		for (auto* u : gv->users())
		{
			auto* ce = dyn_cast<ConstantExpr>(u);
			if (!ce || ce->getOpcode() != Instruction::PtrToInt) continue;
			for (auto* uu : ce->users())
			{
				// The address escapes into arithmetic or a register (an x86
				// `lea` of the table base), not just a fixed-offset access.
				if (isa<Instruction>(uu) && !isa<LoadInst>(uu))
				{
					indexed = true;
				}
			}
		}
		if (!indexed) continue;
		uint64_t es = dl.getTypeAllocSize(et);
		uint64_t size = 0;
		// Several symbols can share an address (labels, mapping symbols):
		// take the largest object size among them.
		bool named = false;
		unsigned long long ss = 0;
		for (const auto* tab : ff ? ff->getSymbolTables() : std::vector<fileformat::SymbolTable*>{})
		{
			for (const auto& sym : *tab)
			{
				unsigned long long sa = 0, sz = 0;
				if (sym->getAddress(sa) && sa == addr && sym->getSize(sz) && sz > 0)
				{
					named = true;
					ss = std::max(ss, sz);
				}
			}
		}
		if (named && ss > es)
		{
			size = ss;
		}
		else if (!named && gv->isConstant())
		{
			auto* seg = img->getSegmentFromAddress(addr);
			if (seg == nullptr) continue;
			uint64_t end = seg->getEndAddress();
			auto next = byAddr.upper_bound(addr);
			if (next != byAddr.end() && next->first < end) end = next->first;
			size = std::min<uint64_t>(end - addr, 1024);
		}
		while (es > 1 && size % es) es /= 2;
		if (size <= es) continue;
		work.emplace_back(gv, size / es, es);
	}
	for (auto& [gv, n, es] : work)
	{
		Type* et = Type::getIntNTy(_module->getContext(), es * 8);
		auto addr = _config->getGlobalAddress(gv);
		auto* at = ArrayType::get(et, n);
		std::vector<Constant*> elems;
		bool le = dl.isLittleEndian();
		std::vector<std::uint8_t> bytes;
		bool have = img->getXBytes(addr, es * n, bytes) && bytes.size() >= es * n;
		for (uint64_t k = 0; k < n; ++k)
		{
			APInt v(et->getIntegerBitWidth(), 0);
			for (uint64_t j = 0; have && j < es; ++j)
			{
				uint64_t b = bytes[k * es + (le ? es - 1 - j : j)];
				v = v.shl(8) | APInt(et->getIntegerBitWidth(), b);
			}
			elems.push_back(ConstantInt::get(et, v));
		}
		auto* ngv = new GlobalVariable(*_module, at, gv->isConstant(),
				gv->getLinkage(), ConstantArray::get(at, elems));
		ngv->takeName(gv);
		gv->replaceAllUsesWith(ngv);
		auto* cgv = _config->getConfigGlobalVariable(gv);
		std::string realName = cgv ? cgv->getName() : std::string();
		gv->eraseFromParent();
		_config->insertGlobalVariable(ngv, addr, false, realName);
	}
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
	// A register global here is the register's value on entry, not its
	// address: `mov %rdi,%rdx; movzbl (%rdx),%edi` reads through rdi's value,
	// and replacing the pointer with @rdi read the register instead.
	if (isa<LoadInst>(inst) && gv && root.ops.size() <= 1
			&& !(_abi && _abi->isRegister(gv)))
	{
		auto* conv = IrModifier::convertConstantToType(gv, val->getType());
		_toRemove.insert(val);
		inst->replaceUsesOfWith(val, conv);
		return;
	}
}

} // namespace bin2llvmir
} // namespace retdec
