/**
 * @file src/container_detect/list_detect.cpp
 * @brief std::list<T> detector — circular doubly-linked list fingerprinting.
 *
 * ## Structural fingerprint
 *
 * libstdc++ std::list uses an intrusive sentinel node whose _next and _prev
 * pointers both point to itself in the empty-list initialisation:
 *
 *   ```cpp
 *   _M_header._M_next = &_M_header;  // circular sentinel
 *   _M_header._M_prev = &_M_header;
 *   ```
 *
 * In IR this produces:
 *   - A Store instruction where the source value is the address of the
 *     destination struct (self-referential write).
 *   - Another such Store for the sibling pointer (same struct, adjacent offset).
 *
 * Regular list nodes are heap-allocated:
 *   - malloc/new call with size = 2*pointer_width + sizeof(T).
 *   - Result pointer is written to two adjacent offsets (prev/next linkage) of
 *     neighbouring nodes.
 *
 * Iteration pattern:
 *   - A loop that loads `node->_next` into the induction variable and compares
 *     it to the sentinel address (equality or pointer comparison).
 *
 * Insert / erase:
 *   - Four pointer stores: update prev/next in the two surrounding nodes.
 *
 * ## Confidence scoring
 *
 *   sentinel self-referential init    +0.35
 *   node heap allocation              +0.25
 *   chain traversal loop              +0.25
 *   four-pointer update (insert/erase)+0.15
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace container_detect {

namespace {

// Count Store instructions in fn.
static int countStores(const ssa::SSAFunction& fn)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
			if (instr && instr->op == ssa::IrInstr::Op::Store) ++n;
	}
	return n;
}

// A sentinel init writes one address -- the header's own -- into two different
// slots of the object living at that address:
//
//   _M_header._M_next = &_M_header;
//   _M_header._M_prev = &_M_header;
//
// so the thing to look for is a pair of adjacent Stores that share a stored
// value and whose destination slots both hang off the variable that value
// names.  The old code claimed to check "both stores reference the same base
// address value" but actually compared a->uses[1] against b->uses[1] + 1, which
// is neither that nor anything else meaningful -- and it read uses[1] behind an
// !uses.empty() guard, so a pair of half-formed single-operand Stores (address
// never renamed) walked off the end of both use-lists.
static bool isSlotOf(const ssa::SSAFunction& fn, ssa::ValueId addr, ssa::ValueId base)
{
	const auto* slot = fn.value(addr);
	const auto* obj = fn.value(base);
	if (!slot || !obj) return false;
	if (slot->kind != ssa::ValueKind::MemRef) return false;
	// memBaseReg is a VarId while `base` is a ValueId; the comparison has to go
	// through the value's originating variable or it is comparing two unrelated
	// numbering spaces, which is exactly the mistake this replaces.
	return slot->memBaseReg != ssa::kInvalidVar && slot->memBaseReg == obj->varId;
}

static bool hasSentinelInit(const ssa::SSAFunction& fn)
{
	if (fn.blockCount() == 0) return false;
	const auto* entry = fn.block(0);
	if (!entry) return false;

	int selfRefStores = 0;
	for (std::size_t i = 0; i + 1 < entry->instrs.size(); ++i)
	{
		const auto* a = entry->instrs[i];
		const auto* b = entry->instrs[i + 1];
		if (!a || !b) continue;
		if (a->op != ssa::IrInstr::Op::Store || b->op != ssa::IrInstr::Op::Store) continue;

		// A Store carries (value, address).  Anything with fewer than two
		// operands has no address to inspect, so there is nothing to compare.
		if (a->uses.size() < 2 || b->uses.size() < 2) continue;

		const ssa::ValueId storedA = a->uses[0].valueId;
		const ssa::ValueId storedB = b->uses[0].valueId;
		const ssa::ValueId addrA = a->uses[1].valueId;
		const ssa::ValueId addrB = b->uses[1].valueId;

		// Same value into two *different* slots: equal stored values alone is a
		// memset, and distinct slots alone is any struct initialiser.
		if (storedA == ssa::kInvalidValue || storedA != storedB) continue;
		if (addrA == ssa::kInvalidValue || addrB == ssa::kInvalidValue) continue;
		if (addrA == addrB) continue;

		if (isSlotOf(fn, addrA, storedA) && isSlotOf(fn, addrB, storedB))
		{
			++selfRefStores;
		}
	}
	return selfRefStores >= 1;
}

// Node heap allocation: a malloc/new call followed by a pointer store.
static bool hasNodeAlloc(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
			const auto& cn = instr->calleeName;
			if (cn == "malloc" || cn == "_Znwm" || cn == "operator new" || cn.find("allocate") != std::string::npos)
				return true;
		}
	}
	return false;
}

// Chain traversal: a loop that loads a pointer from an offset (the _next slot)
// and compares it to another pointer (the sentinel or nullptr).
// In practice we detect: ≥1 Load + ≥1 Compare + ≥1 back-edge.
static bool hasChainTraversal(const ssa::SSAFunction& fn)
{
	int loads = 0, compares = 0, backEdges = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr) continue;
			if (instr->op == ssa::IrInstr::Op::Load) ++loads;
			if (instr->op == ssa::IrInstr::Op::Compare) ++compares;
		}
		// Back-edge: a successor block with a smaller index.
		for (uint32_t succ: blk->succs)
			if (succ <= b) ++backEdges;
	}
	return loads >= 1 && compares >= 1 && backEdges >= 1;
}

// Four-pointer update: insert or erase modifies 4 pointer slots.
// Approximation: ≥4 Store instructions in the function.
static bool hasFourPtrUpdate(const ssa::SSAFunction& fn)
{
	return countStores(fn) >= 4;
}

} // anonymous namespace

// ─── ListDetector ────────────────────────────────────────────────────────────

ListEvidence ListDetector::analyseStructure(const ssa::SSAFunction& fn) const
{
	ListEvidence ev;
	ev.hasSentinelNode = hasSentinelInit(fn);
	ev.hasNodeAlloc = hasNodeAlloc(fn);
	ev.hasChainTraversal = hasChainTraversal(fn);
	ev.hasFourPtrUpdate = hasFourPtrUpdate(fn);
	ev.found = ev.hasSentinelNode || (ev.hasNodeAlloc && ev.hasChainTraversal);
	ev.confidence = scoreEvidence(ev);
	return ev;
}

float ListDetector::scoreEvidence(const ListEvidence& ev) const
{
	float s = 0.0f;
	if (ev.hasSentinelNode) s += 0.35f;
	if (ev.hasNodeAlloc) s += 0.25f;
	if (ev.hasChainTraversal) s += 0.25f;
	if (ev.hasFourPtrUpdate) s += 0.15f;
	return s > 1.0f ? 1.0f : s;
}

ContainerResult ListDetector::detect(const ssa::SSAFunction& fn) const
{
	ContainerResult result;
	result.kind = ContainerKind::List;

	auto ev = analyseStructure(fn);
	result.confidence = ev.confidence;

	if (ev.confidence < 0.10f) return result;

	result.emittedType = "std::list<int>";
	if (ev.hasNodeAlloc && !ev.hasSentinelNode) result.emittedType = "evidence:symbol_name " + result.emittedType;
	result.elementType.kind = RecoveredType::Kind::Int32;

	if (ev.hasChainTraversal)
	{
		AccessPattern ap;
		ap.kind = AccessKind::Iterate;
		ap.emitted = "for (auto& e : lst)";
		result.accessPatterns.push_back(ap);
	}
	if (ev.hasNodeAlloc)
	{
		AccessPattern ap;
		ap.kind = AccessKind::PushBack;
		ap.emitted = "lst.push_back(elem)";
		result.accessPatterns.push_back(ap);
	}
	if (ev.hasFourPtrUpdate)
	{
		AccessPattern ap;
		ap.kind = AccessKind::Erase;
		ap.emitted = "lst.erase(it)";
		result.accessPatterns.push_back(ap);
	}

	return result;
}

} // namespace container_detect
} // namespace retdec
