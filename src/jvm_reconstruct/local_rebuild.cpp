/**
 * @file src/jvm_reconstruct/local_rebuild.cpp
 * @brief Local variable reconstruction from JVM bytecode.
 */

#include "retdec/utils/bounds.h"
#include <memory>
#include "retdec/jvm_reconstruct/local_rebuild.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── LocalRebuilder ───────────────────────────────────────────────────────────

LocalRebuilder::LocalRebuilder(LocalRebuildOptions opts): opts_(opts) {}

// ─── JVM descriptor → BcType ─────────────────────────────────────────────────

/// The array-dimension ceiling. JVMS 4.4.1 caps a field descriptor at 255
/// dimensions; a descriptor asking for more is malformed, not deep.
static constexpr std::size_t kMaxArrayDimensions = 255;

/// The non-array part of a descriptor: everything descriptorToType understands
/// once the leading '[' run has been counted off.
static BcType elementDescriptorToType(const std::string& desc)
{
	if (desc.empty()) return types::Void();
	switch (desc[0])
	{
	case 'V': return types::Void();
	case 'Z': return types::Bool();
	case 'B': return types::Byte();
	case 'S': return types::Short();
	case 'C': return types::Char();
	case 'I': return types::Int();
	case 'J': return types::Long();
	case 'F': return types::Float();
	case 'D': return types::Double();
	case 'L': {
		std::string cls = desc.substr(1);
		if (!cls.empty() && cls.back() == ';') cls.pop_back();
		for (char& c: cls)
			if (c == '/') c = '.';
		BcRefType ref;
		ref.kind = BcRefKind::Class;
		ref.className = cls;
		return BcType{ref};
	}
	default: return types::Int();
	}
}

/// A JVM field descriptor as a BcType.
///
/// The '[' arm used to recurse on `desc.substr(1)`, so the recursion depth was
/// the number of leading brackets the .class file asked for -- and the
/// descriptor comes out of the constant pool, where a `[` run is one byte
/// each. This is the same defect that was fixed in DexClassParser, in an exact
/// second copy of the function; the adversarial verify pass measured this one
/// still live: 20,000 brackets returns, 40,000 gives SIGSEGV at -O1.
///
/// Counting the run and building the nest with a loop leaves no recursion at
/// all, and the bound is applied before a single node is allocated, so an
/// over-deep descriptor costs nothing to refuse. Within the bound the result is
/// bit-for-bit the type the recursion produced -- the nest is still built
/// innermost-first.
BcType LocalRebuilder::descriptorToType(const std::string& desc)
{
	std::size_t dims = 0;
	while (dims < desc.size() && desc[dims] == '[')
	{
		++dims;
	}
	if (dims > kMaxArrayDimensions)
	{
		return types::Int();
	}

	BcType type = elementDescriptorToType(desc.substr(dims));
	for (std::size_t i = 0; i < dims; ++i)
	{
		BcRefType ref;
		ref.kind = BcRefKind::Array;
		ref.elementType = std::make_shared<BcType>(std::move(type));
		type = BcType{ref};
	}
	return type;
}

// ─── Slot type inference ──────────────────────────────────────────────────────

std::unordered_map<uint32_t, BcType>
LocalRebuilder::inferSlotTypes(const BcCFG& cfg, const StackSimResult& simResult) const
{
	std::unordered_map<uint32_t, BcType> slotTypes;

	for (const auto& blk: cfg.blocks())
	{
		for (const auto& insn: blk.instrs)
		{
			auto it = simResult.instrInfo.find(insn.id);
			if (it == simResult.instrInfo.end()) continue;

			// For StoreLocal: the operand is the slot, the pop is the type.
			if (insn.opcode == BcOpcode::StoreLocal && !it->second.pops.empty())
			{
				if (!insn.operands.empty())
				{
					if (auto* lop = std::get_if<BcLocalOperand>(&insn.operands[0]))
					{
						uint32_t localIdx = lop->index;
						uint32_t popSlot = it->second.pops.front();
						if (const StackSlot* s = simResult.slot(popSlot)) slotTypes[localIdx] = s->type;
					}
				}
			}

			// For LoadLocal: the push type is the local's type.
			if (insn.opcode == BcOpcode::LoadLocal && !it->second.pushes.empty())
			{
				if (!insn.operands.empty())
				{
					if (auto* lop = std::get_if<BcLocalOperand>(&insn.operands[0]))
					{
						uint32_t localIdx = lop->index;
						uint32_t pushSlot = it->second.pushes.front();
						if (const StackSlot* s = simResult.slot(pushSlot))
							if (!slotTypes.count(localIdx)) slotTypes[localIdx] = s->type;
					}
				}
			}
		}
	}
	return slotTypes;
}

// ─── Naming ───────────────────────────────────────────────────────────────────

std::string LocalRebuilder::nameSlot(uint32_t slotIdx, const BcType& type, bool isParam, uint32_t paramOrdinal) const
{
	if (isParam && opts_.nameParameters)
	{
		// Use type-based prefix for parameters.
		std::string prefix = "param";
		if (type.isPrim())
		{
			switch (type.prim().kind)
			{
			case BcPrimKind::Int: prefix = "i"; break;
			case BcPrimKind::Long: prefix = "l"; break;
			case BcPrimKind::Float: prefix = "f"; break;
			case BcPrimKind::Double: prefix = "d"; break;
			case BcPrimKind::Bool: prefix = "b"; break;
			default: prefix = "p"; break;
			}
		}
		else if (type.isRef())
		{
			// Use simple class name lowercased as prefix.
			//
			// Both separators, and the nested-class marker. BcRefType::className
			// is documented as internal form ("java/lang/String") and that is
			// what the JVM front end stores -- JvmSignatureParser builds
			// Class(name) from the raw descriptor name without converting the
			// separator. rfind('.') found nothing in it, so the whole internal
			// name survived as the prefix and the "name" contained slashes,
			// which is not a Java identifier. descriptorToType in this same
			// file normalises '/' to '.', which is why LVT-derived types
			// worked and descriptor-derived ones did not.
			std::string cls = type.ref().className;
			size_t dot = cls.find_last_of("./$");
			if (dot != std::string::npos) cls = cls.substr(dot + 1);
			if (!cls.empty())
			{
				cls[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(cls[0])));
				prefix = cls;
			}
		}
		return prefix + std::to_string(paramOrdinal);
	}

	if (opts_.generateSyntheticNames)
	{
		// Generate a name based on type + slot index.
		std::string prefix = "v";
		if (type.isPrim())
		{
			switch (type.prim().kind)
			{
			case BcPrimKind::Int: prefix = "i"; break;
			case BcPrimKind::Long: prefix = "l"; break;
			case BcPrimKind::Float: prefix = "f"; break;
			case BcPrimKind::Double: prefix = "d"; break;
			case BcPrimKind::Bool: prefix = "b"; break;
			default: prefix = "v"; break;
			}
		}
		return prefix + std::to_string(slotIdx);
	}
	return "v" + std::to_string(slotIdx);
}

// ─── Main rebuild ─────────────────────────────────────────────────────────────

LocalRebuildResult LocalRebuilder::rebuild(
	const BcMethod& method,
	const BcCFG& cfg,
	const StackSimResult& simResult,
	const CoalesceResult& coalesceResult,
	const std::vector<LVTEntry>& lvtEntries)
{
	LocalRebuildResult result;

	// Build a map from JVM slot index → LVTEntry (if available).
	std::unordered_map<uint32_t, const LVTEntry*> lvtMap;
	if (opts_.useDebugTables)
	{
		for (const auto& entry: lvtEntries)
			lvtMap[entry.index] = &entry;
	}

	// Infer types from bytecode where debug info is absent.
	auto inferredTypes = inferSlotTypes(cfg, simResult);

	// Determine the maximum local slot index referenced.
	uint32_t maxSlot = 0;
	for (const auto& kv: inferredTypes)
		maxSlot = std::max(maxSlot, kv.first);
	for (const auto& entry: lvtEntries)
		maxSlot = std::max(maxSlot, static_cast<uint32_t>(entry.index));

	// Determine how many parameter slots are consumed.
	// Slot 0 = `this` for instance methods; then parameter slots follow.
	bool isStatic = hasFlag(method.access, BcAccess::Static) || method.isStaticInit;
	uint32_t paramSlot = isStatic ? 0 : 1;
	std::vector<BcType> paramTypes;
	if (!isStatic)
	{
		// 'this' type — derive from the owning class name if possible.
		BcRefType thisRef;
		thisRef.kind = BcRefKind::Class;
		thisRef.className = "this"; // placeholder
		paramTypes.push_back(BcType{thisRef});
	}
	for (const auto& pt: method.descriptor.params)
		if (pt) paramTypes.push_back(*pt);

	// Map: slot index → BcLocalVar index in result.locals.
	std::unordered_map<uint32_t, uint32_t> slotToIdx;

	// Add 'this' if instance method.
	if (!isStatic)
	{
		BcLocalVar lv;
		lv.index = 0;
		lv.name = "this";
		lv.isParam = true;
		lv.type = paramTypes[0];
		lv.startOffset = 0;
		lv.endOffset = UINT32_MAX;
		slotToIdx[0] = static_cast<uint32_t>(result.locals.size());
		result.slotToLocal[0] = static_cast<uint32_t>(result.locals.size());
		result.locals.push_back(std::move(lv));
	}

	// Add parameters.
	uint32_t paramOrdinal = 0;
	for (uint32_t pi = 0; pi < method.descriptor.params.size(); ++pi)
	{
		uint32_t slot = paramSlot;
		const BcType& ptype = *method.descriptor.params[pi];

		BcLocalVar lv;
		lv.index = slot;
		lv.isParam = true;
		lv.type = ptype;
		lv.startOffset = 0;
		lv.endOffset = UINT32_MAX;

		if (opts_.useDebugTables && lvtMap.count(slot))
			lv.name = lvtMap.at(slot)->name;
		else
			lv.name = nameSlot(slot, ptype, true, paramOrdinal++);

		slotToIdx[slot] = static_cast<uint32_t>(result.locals.size());
		result.slotToLocal[slot] = static_cast<uint32_t>(result.locals.size());
		result.locals.push_back(std::move(lv));

		// Wide types consume two slots.
		bool wide =
			(ptype.isPrim()
			 && (ptype.prim() == BcPrimType{BcPrimKind::Long} || ptype.prim() == BcPrimType{BcPrimKind::Double}));
		paramSlot += wide ? 2 : 1;
	}

	// Add other local variables (non-parameter slots).
	std::set<uint32_t> paramSlots;
	{
		uint32_t ps = isStatic ? 0 : 1;
		if (!isStatic) paramSlots.insert(0);
		for (const auto& pt: method.descriptor.params)
		{
			paramSlots.insert(ps);
			bool wide =
				(pt && pt->isPrim()
				 && (pt->prim() == BcPrimType{BcPrimKind::Long} || pt->prim() == BcPrimType{BcPrimKind::Double}));
			ps += wide ? 2 : 1;
		}
	}

	for (uint32_t slot = 0; slot <= maxSlot + 1; ++slot)
	{
		if (paramSlots.count(slot)) continue;
		if (!inferredTypes.count(slot) && !lvtMap.count(slot)) continue;

		BcLocalVar lv;
		lv.index = slot;
		lv.isParam = false;

		if (opts_.useDebugTables && lvtMap.count(slot))
		{
			const LVTEntry* entry = lvtMap.at(slot);
			lv.name = entry->name;
			lv.type = descriptorToType(entry->descriptor);
			lv.startOffset = entry->startPc;
			// `startPc + length` are two u2 fields the .class file supplies, so
			// the sum reaches 131070 and the region can end before it begins on
			// a hostile file. bounds::addFits never forms the sum; a pair that
			// does not fit describes no region, so the variable gets an empty
			// one at its start rather than a wrapped extent. The same two lines
			// were fixed in JvmLifter::wireExceptions; this is the third module
			// to carry them.
			lv.endOffset =
				utils::bounds::addFits(entry->startPc, entry->length) ? entry->startPc + entry->length : entry->startPc;
		}
		else
		{
			BcType t = inferredTypes.count(slot) ? inferredTypes.at(slot) : types::Int();
			lv.type = t;
			lv.name = nameSlot(slot, t, false, 0);
			lv.startOffset = 0;
			lv.endOffset = UINT32_MAX;
		}

		slotToIdx[slot] = static_cast<uint32_t>(result.locals.size());
		result.slotToLocal[slot] = static_cast<uint32_t>(result.locals.size());
		result.locals.push_back(std::move(lv));
	}

	(void)coalesceResult; // used by the CFG rewriter, not needed here
	return result;
}

// ─── ExceptionVarIntroducer ───────────────────────────────────────────────────

void ExceptionVarIntroducer::introduce(
	BcCFG& cfg, BcMethod& method, const StackSimResult& simResult, LocalRebuildResult& localResult)
{
	for (const auto& eh: cfg.handlers())
	{
		uint32_t hblk = eh.handlerBlock;
		if (hblk >= cfg.blockCount()) continue;

		BcBasicBlock& blk = cfg.block(hblk);
		blk.isExceptionHandler = true;

		// Create a local variable for the exception if not already present.
		uint32_t exSlot = 1000 + hblk; // synthetic slot index beyond normal range
		if (localResult.slotToLocal.count(exSlot)) continue;

		BcLocalVar exVar;
		exVar.index = exSlot;
		BcType exType;
		if (eh.catchType.has_value())
			exType = *eh.catchType;
		else
		{
			BcRefType t;
			t.kind = BcRefKind::Class;
			t.className = "java.lang.Throwable";
			exType = BcType{t};
		}
		exVar.type = exType;
		exVar.name = "ex" + std::to_string(hblk);
		exVar.isParam = false;
		exVar.startOffset = 0;
		exVar.endOffset = UINT32_MAX;

		uint32_t localIdx = static_cast<uint32_t>(localResult.locals.size());
		localResult.slotToLocal[exSlot] = localIdx;
		localResult.locals.push_back(std::move(exVar));
		method.locals = localResult.locals;

		// Insert a synthetic StoreLocal at the start of the handler block.
		BcInstruction store;
		store.id = static_cast<uint32_t>(blk.instrs.size() + 1000);
		store.offset = 0;
		store.opcode = BcOpcode::StoreLocal;
		// The JVM slot, not the index into localResult.locals.
		//
		// Every consumer reads BcLocalOperand::index as a slot: ExprContext
		// keys localNames by BcLocalVar::index, which is exSlot above, and
		// JavaExprEmitter's StoreLocal case looks the operand up in that map.
		// Passing localIdx named whichever local happened to sit at JVM slot
		// `localIdx` -- an unrelated variable, usually an int -- so the catch
		// body assigned to that one and `ex<blk>` was declared and never
		// assigned.
		store.operands.push_back(BcLocalOperand{exSlot});

		blk.instrs.insert(blk.instrs.begin(), std::move(store));
	}
}

} // namespace jvm_reconstruct
} // namespace retdec
