/**
 * @file src/algo_recover/binary_search_detect.cpp
 * @brief Binary search detector — SSA def-use query (audit A3).
 *
 * Required invariant (prefer proving all four; precision over recall):
 *   1. Midpoint: a range is halved — Add(lo, hi) then Shr 1 / Div 2,
 *      or the overflow-safe form lo + ((hi - lo) >> 1).
 *   2. Load of an element at that midpoint (or an index derived from it).
 *   3. Compare of that loaded value (or a value derived from the load)
 *      against a loop-invariant target.
 *   4. Successors update lo or hi from the midpoint (phi operand, Store,
 *      or Add/Sub of the mid SSA value).
 *
 * If phi/bound wiring is too thin to prove (4), the strongest conservative
 * approximation still required is def-use of the Shr/Div result: mid must
 * be used as a Load index and by a Compare or a bound Add/Sub.
 *
 * Whole-function opcode counts and function-name hints are not used.
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace algo_recover {

namespace {

using ssa::BlockId;
using ssa::IrInstr;
using ssa::IrValue;
using ssa::PhiNode;
using ssa::SSAFunction;
using ssa::ValueId;
using ssa::kInvalidBlock;
using ssa::kInvalidValue;

struct UseDef {
	std::unordered_map<ValueId, std::vector<const IrInstr*>> users;
	std::unordered_map<ValueId, const IrInstr*> defOf;
	std::unordered_map<ValueId, const PhiNode*> phiDefOf;
	std::unordered_map<ValueId, std::vector<const PhiNode*>> phiUsers;
};

static UseDef buildUseDef(const SSAFunction& fn)
{
	UseDef ud;
	for (const auto& blk : fn.blocks()) {
		if (!blk) continue;
		for (const IrInstr* i : blk->instrs) {
			if (!i) continue;
			if (i->defValue != kInvalidValue)
				ud.defOf[i->defValue] = i;
			for (const auto& u : i->uses) {
				if (u.valueId != kInvalidValue)
					ud.users[u.valueId].push_back(i);
			}
		}
	}
	for (const auto& p : fn.phis()) {
		if (!p) continue;
		if (p->result != kInvalidValue)
			ud.phiDefOf[p->result] = p.get();
		for (const auto& op : p->operands) {
			if (op.second != kInvalidValue)
				ud.phiUsers[op.second].push_back(p.get());
		}
	}
	for (const auto& vp : fn.values()) {
		if (!vp) continue;
		if (vp->defInstr && vp->id != kInvalidValue)
			ud.defOf.emplace(vp->id, vp->defInstr);
		if (vp->defPhi && vp->id != kInvalidValue)
			ud.phiDefOf.emplace(vp->id, vp->defPhi);
	}
	return ud;
}

static bool hasBackEdge(const SSAFunction& fn)
{
	for (const auto& blk : fn.blocks()) {
		if (!blk) continue;
		for (BlockId s : blk->succs)
			if (s <= blk->id) return true;
	}
	return false;
}

/// Natural-loop blocks from back-edges (succ id <= src id). Preds are
/// used when present; otherwise the latch and header alone are kept.
static std::unordered_set<BlockId> loopBlocks(const SSAFunction& fn)
{
	std::unordered_set<BlockId> loop;
	for (const auto& blk : fn.blocks()) {
		if (!blk) continue;
		for (BlockId header : blk->succs) {
			if (header > blk->id) continue;
			loop.insert(header);
			loop.insert(blk->id);
			std::vector<BlockId> work{blk->id};
			std::unordered_set<BlockId> seen{header, blk->id};
			while (!work.empty()) {
				const BlockId cur = work.back();
				work.pop_back();
				const auto* cb = fn.block(cur);
				if (!cb) continue;
				for (BlockId p : cb->preds) {
					if (seen.insert(p).second) {
						loop.insert(p);
						work.push_back(p);
					}
				}
			}
		}
	}
	return loop;
}

static const IrValue* asImm(const SSAFunction& fn, ValueId id)
{
	const IrValue* v = fn.value(id);
	return (v && v->kind == ssa::ValueKind::Immediate) ? v : nullptr;
}

static bool isHalving(const SSAFunction& fn, const IrInstr& i)
{
	if (i.op != IrInstr::Op::Shr &&
	    i.op != IrInstr::Op::Sar &&
	    i.op != IrInstr::Op::Div)
		return false;
	for (const auto& u : i.uses) {
		const IrValue* imm = asImm(fn, u.valueId);
		if (!imm) continue;
		if ((i.op == IrInstr::Op::Shr || i.op == IrInstr::Op::Sar) &&
		    imm->imm == 1)
			return true;
		if (i.op == IrInstr::Op::Div && imm->imm == 2)
			return true;
	}
	return false;
}

static const IrInstr* defInstrOf(const UseDef& ud, ValueId id)
{
	auto it = ud.defOf.find(id);
	return it == ud.defOf.end() ? nullptr : it->second;
}

static BlockId defBlockOf(const SSAFunction& fn, const UseDef& ud, ValueId id)
{
	if (const IrInstr* d = defInstrOf(ud, id))
		return d->block;
	auto pit = ud.phiDefOf.find(id);
	if (pit != ud.phiDefOf.end() && pit->second)
		return pit->second->block;
	const IrValue* v = fn.value(id);
	if (v && v->defInstr) return v->defInstr->block;
	if (v && v->defPhi) return v->defPhi->block;
	return kInvalidBlock;
}

static bool isLoopInvariant(
		const SSAFunction& fn,
		const UseDef& ud,
		ValueId id,
		const std::unordered_set<BlockId>& loop)
{
	const IrValue* v = fn.value(id);
	if (!v) return false;
	if (v->kind == ssa::ValueKind::Immediate) return true;
	const BlockId b = defBlockOf(fn, ud, id);
	if (b == kInvalidBlock)
		return true; // parameter / no in-function def
	if (loop.empty()) return false;
	return loop.find(b) == loop.end();
}

static bool isIndexArith(IrInstr::Op op)
{
	return op == IrInstr::Op::Add || op == IrInstr::Op::Sub ||
	       op == IrInstr::Op::Shl || op == IrInstr::Op::Mul ||
	       op == IrInstr::Op::Assign;
}

/// mid and values computed from it (scaled index, base+mid, mid±1).
static std::unordered_set<ValueId> derivedFrom(
		const UseDef& ud, ValueId root, int hops)
{
	std::unordered_set<ValueId> out;
	if (root == kInvalidValue) return out;
	out.insert(root);
	std::vector<ValueId> frontier{root};
	for (int h = 0; h < hops; ++h) {
		std::vector<ValueId> next;
		for (ValueId v : frontier) {
			auto it = ud.users.find(v);
			if (it == ud.users.end()) continue;
			for (const IrInstr* i : it->second) {
				if (!i || !isIndexArith(i->op)) continue;
				if (i->defValue == kInvalidValue) continue;
				if (out.insert(i->defValue).second)
					next.push_back(i->defValue);
			}
		}
		frontier.swap(next);
	}
	return out;
}

static bool operandIs(const IrInstr& i, ValueId id)
{
	for (const auto& u : i.uses)
		if (u.valueId == id) return true;
	return false;
}

static bool addOrSubFeeds(const UseDef& ud, const IrInstr& half, IrInstr::Op want)
{
	for (const auto& u : half.uses) {
		const IrInstr* d = defInstrOf(ud, u.valueId);
		if (d && d->op == want) return true;
	}
	return false;
}

static bool halfUsedByAdd(const UseDef& ud, ValueId mid)
{
	auto it = ud.users.find(mid);
	if (it == ud.users.end()) return false;
	for (const IrInstr* i : it->second)
		if (i && i->op == IrInstr::Op::Add) return true;
	return false;
}

struct MidEvidence {
	bool hasHalving = false;
	bool hasRangeAdd = false;
	bool midUsedAsLoadIndex = false;
	bool loadCompared = false;
	bool targetInvariant = false;
	bool midUsedByCompare = false;
	bool midUsedByBoundArith = false;
	bool midUpdatesBound = false;
};

static MidEvidence analyseMid(
		const SSAFunction& fn,
		const UseDef& ud,
		const IrInstr& half,
		const std::unordered_set<BlockId>& loop)
{
	MidEvidence ev;
	ev.hasHalving = true;

	const bool addFeeds = addOrSubFeeds(ud, half, IrInstr::Op::Add);
	const bool subFeeds = addOrSubFeeds(ud, half, IrInstr::Op::Sub);
	ev.hasRangeAdd = addFeeds || (subFeeds && halfUsedByAdd(ud, half.defValue));

	const ValueId mid = half.defValue;
	const auto fromMid = derivedFrom(ud, mid, 2);

	const IrInstr* loadI = nullptr;
	for (ValueId v : fromMid) {
		auto it = ud.users.find(v);
		if (it == ud.users.end()) continue;
		for (const IrInstr* i : it->second) {
			if (i && i->op == IrInstr::Op::Load) {
				loadI = i;
				break;
			}
		}
		if (loadI) break;
	}
	ev.midUsedAsLoadIndex = loadI != nullptr;

	if (loadI && loadI->defValue != kInvalidValue) {
		const auto fromLoad = derivedFrom(ud, loadI->defValue, 2);
		auto uit = ud.users.find(loadI->defValue);
		std::vector<const IrInstr*> cmps;
		if (uit != ud.users.end()) {
			for (const IrInstr* i : uit->second)
				if (i && i->op == IrInstr::Op::Compare)
					cmps.push_back(i);
		}
		for (ValueId v : fromLoad) {
			if (v == loadI->defValue) continue;
			auto it = ud.users.find(v);
			if (it == ud.users.end()) continue;
			for (const IrInstr* i : it->second)
				if (i && i->op == IrInstr::Op::Compare)
					cmps.push_back(i);
		}
		for (const IrInstr* cmp : cmps) {
			ev.loadCompared = true;
			for (const auto& u : cmp->uses) {
				if (fromLoad.count(u.valueId)) continue;
				if (u.valueId == loadI->defValue) continue;
				if (isLoopInvariant(fn, ud, u.valueId, loop))
					ev.targetInvariant = true;
			}
		}
	}

	auto mit = ud.users.find(mid);
	if (mit != ud.users.end()) {
		for (const IrInstr* i : mit->second) {
			if (!i) continue;
			if (i->op == IrInstr::Op::Compare)
				ev.midUsedByCompare = true;
			if (i->op == IrInstr::Op::Add || i->op == IrInstr::Op::Sub) {
				// Skip the range-sum that *feeds* this half (cannot).
				// Skip the (lo+hi) add; it does not use mid.
				// Skip only if this add's result is an operand of half.
				if (i->defValue != kInvalidValue && operandIs(half, i->defValue))
					continue;
				ev.midUsedByBoundArith = true;
			}
			if (i->op == IrInstr::Op::Store)
				ev.midUpdatesBound = true;
		}
	}

	if (ud.phiUsers.count(mid))
		ev.midUpdatesBound = true;
	for (ValueId v : fromMid) {
		if (v == mid) continue;
		if (ud.phiUsers.count(v))
			ev.midUpdatesBound = true;
	}
	if (ev.midUsedByBoundArith)
		ev.midUpdatesBound = true;

	return ev;
}

static float score(const MidEvidence& ev)
{
	const bool inv1 = ev.hasHalving && ev.hasRangeAdd;
	const bool inv2 = ev.midUsedAsLoadIndex;
	const bool inv3 = ev.loadCompared && ev.targetInvariant;
	const bool inv4 = ev.midUpdatesBound;

	if (inv1 && inv2 && inv3 && inv4)
		return 0.90f;

	// Conservative: def-use of the shr/div result to a Load index and
	// to a Compare (of the load or of mid) or a bound Add/Sub.
	const bool conservative =
		ev.hasHalving && inv2 &&
		(inv3 || ev.midUsedByCompare || ev.midUsedByBoundArith);
	if (conservative)
		return 0.55f;

	return 0.0f;
}

} // anonymous namespace

AlgorithmResult BinarySearchDetector::detect(const SSAFunction& fn) const
{
	AlgorithmResult result;
	result.kind = AlgorithmKind::BinarySearch;

	if (!hasBackEdge(fn))
		return result;

	const UseDef ud = buildUseDef(fn);
	const auto loop = loopBlocks(fn);

	float best = 0.0f;
	for (const auto& blk : fn.blocks()) {
		if (!blk) continue;
		for (const IrInstr* i : blk->instrs) {
			if (!i || !isHalving(fn, *i)) continue;
			if (i->defValue == kInvalidValue) continue;
			const MidEvidence ev = analyseMid(fn, ud, *i, loop);
			const float s = score(ev);
			if (s > best) best = s;
		}
	}

	result.confidence = best;
	if (best >= 0.75f)
		result.tier = EmissionTier::High;
	else if (best >= 0.45f)
		result.tier = EmissionTier::Medium;
	else
		result.tier = EmissionTier::Low;

	if (best < 0.45f)
		return result;

	if (result.tier == EmissionTier::High)
		result.emittedForm = "std::binary_search(first, last, value);";
	else
		result.emittedForm = "/* binary_search? */ midpoint load-compare loop";
	return result;
}

} // namespace algo_recover
} // namespace retdec
