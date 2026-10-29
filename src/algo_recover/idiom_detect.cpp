/**
 * @file src/algo_recover/idiom_detect.cpp
 * @brief Classic C idiom detectors for algorithm-recovery F1 (atoi, BFS, varint, …).
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace algo_recover {

namespace {

bool hasConstant(const ssa::SSAFunction& fn, uint64_t value)
{
	for (const auto& v: fn.values())
		if (v && v->kind == ssa::ValueKind::Immediate && v->imm == value) return true;
	return false;
}

bool hasBackEdge(const ssa::SSAFunction& fn)
{
	return ssa::hasCycle(fn);
}

int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
	int n = 0;
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == op) ++n;
	}
	return n;
}

bool hasSelfCall(const ssa::SSAFunction& fn)
{
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == ssa::IrInstr::Op::Call && i->calleeName == fn.name()) return true;
	}
	return false;
}

template <typename F>
void forEachLoopInstr(const ssa::SSAFunction& fn, F&& f);
bool immOperand(const ssa::SSAFunction& fn, const ssa::IrInstr& i, uint64_t value);

bool hasVarintLoop(const ssa::SSAFunction& fn)
{
	const bool mask7f = hasConstant(fn, 0x7Fu);
	const bool mask80 = hasConstant(fn, 0x80u);
	const bool shr7 = hasConstant(fn, 7u) && countOp(fn, ssa::IrInstr::Op::Shr) >= 1;
	if (hasBackEdge(fn) && mask7f && shr7 && (mask80 || countOp(fn, ssa::IrInstr::Op::And) >= 1)) return true;
	// After truncation to a byte the `& 0x7F` disappears, and the shift is
	// lifted as an unsigned divide by 128: inside the loop, the continuation
	// bit ORed in and the value shifted down by seven.
	bool cont = false, down7 = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if (i.op == ssa::IrInstr::Op::Or && immOperand(fn, i, 0x80u)) cont = true;
		if (((i.op == ssa::IrInstr::Op::Shr || i.op == ssa::IrInstr::Op::Sar) && immOperand(fn, i, 7u))
			|| (i.op == ssa::IrInstr::Op::Div && immOperand(fn, i, 0x80u)))
			down7 = true;
	});
	return cont && down7;
}

/// A byte loaded from memory and tested against zero: `while (*p)`.
///
/// This is what distinguishes a string walk from any other loop, and it needs
/// the def-use graph to see -- the loaded value has to be the one the compare
/// reads. Asking instead for "a loop, the constant 0 somewhere, at least one
/// load, at least one compare, no multiply" describes nearly every loop ever
/// written; it only looked selective while buildSsaModule attached no operands,
/// so `hasConstant(fn, 0)` was usually false by accident. With the def-use
/// graph in place that predicate fired on all nine ci_core binaries at once
/// and took mean_f1 from 0.2712 to 0.1957.
bool hasByteTestedAgainstZero(const ssa::SSAFunction& fn)
{
	std::vector<ssa::ValueId> byteLoads;
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i || i->op != ssa::IrInstr::Op::Load) continue;
			if (i->defValue == ssa::kInvalidValue) continue;

			bool byte = false;
			for (const auto& u: i->uses)
			{
				const ssa::IrValue* v = fn.value(u.valueId);
				if (v && v->kind == ssa::ValueKind::MemRef && v->memWidth == 1) byte = true;
			}
			const ssa::IrValue* d = fn.value(i->defValue);
			if (d && d->width == 8) byte = true;
			if (byte) byteLoads.push_back(i->defValue);
		}
	}
	if (byteLoads.empty()) return false;

	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i || i->op != ssa::IrInstr::Op::Compare) continue;
			bool readsByte = false;
			bool againstZero = false;
			for (const auto& u: i->uses)
			{
				if (std::find(byteLoads.begin(), byteLoads.end(), u.valueId) != byteLoads.end())
				{
					readsByte = true;
				}
				const ssa::IrValue* v = fn.value(u.valueId);
				if (v && v->kind == ssa::ValueKind::Immediate && v->imm == 0u) againstZero = true;
			}
			if (readsByte && againstZero) return true;
		}
	}
	return false;
}

// The byte Load a value comes from, through at most two width casts.
const ssa::IrInstr* byteLoadOf(const ssa::SSAFunction& fn, ssa::ValueId id, int depth = 0)
{
	const ssa::IrValue* v = fn.value(id);
	// A byte kept in a register across iterations (gcc -O2 strcmp: the
	// next `*b` is loaded at the bottom of the loop and the first one is a
	// constant) reaches the compare through a phi; any loaded incoming byte
	// counts.
	if (v && !v->defInstr && v->defPhi && depth < 2)
	{
		for (const auto& op: v->defPhi->operands)
			if (op.second != id)
				if (const auto* d = byteLoadOf(fn, op.second, depth + 1)) return d;
		return nullptr;
	}
	const ssa::IrInstr* d = v ? v->defInstr : nullptr;
	for (int hops = 0; d && d->op == ssa::IrInstr::Op::Assign && !d->uses.empty() && hops < 4; ++hops)
	{
		v = fn.value(d->uses[0].valueId);
		d = v ? v->defInstr : nullptr;
	}
	if (d && d->op == ssa::IrInstr::Op::Phi && depth < 2)
	{
		for (const auto& u: d->uses)
			if (u.valueId != d->defValue)
				if (const auto* l = byteLoadOf(fn, u.valueId, depth + 1)) return l;
		return nullptr;
	}
	if (!d || d->op != ssa::IrInstr::Op::Load) return nullptr;
	// After reg2mem a loop-carried byte lives in a register slot: a wide
	// reload of a slot whose stores are all bytes loaded from memory.
	if (depth < 2)
	{
		ssa::ValueId slot = ssa::kInvalidValue;
		for (const auto& u: d->uses)
			if (const auto* a = fn.value(u.valueId); a && a->kind != ssa::ValueKind::MemRef && !a->defInstr)
				slot = u.valueId;
		if (slot != ssa::kInvalidValue)
			for (const auto& blk: fn.blocks())
				if (blk)
					for (const auto* st: blk->instrs)
					{
						if (!st || st->op != ssa::IrInstr::Op::Store) continue;
						ssa::ValueId val = ssa::kInvalidValue;
						bool toSlot = false;
						for (const auto& u: st->uses)
						{
							if (u.operandIndex == 1)
								toSlot = toSlot || u.valueId == slot;
							else
								val = u.valueId;
						}
						if (toSlot && val != ssa::kInvalidValue)
							if (const auto* l = byteLoadOf(fn, val, depth + 1)) return l;
					}
	}
	bool byte = false;
	for (const auto& u: d->uses)
		if (const auto* m = fn.value(u.valueId); m && m->kind == ssa::ValueKind::MemRef && m->memWidth == 1) byte = true;
	if (const auto* r = fn.value(d->defValue); r && r->width == 8) byte = true;
	return byte ? d : nullptr;
}

// Two different bytes compared with each other: `*a == *b`.  A strlen tests
// one byte against zero; a strcmp also tests it against the other string's.
bool hasBytePairCompare(const ssa::SSAFunction& fn)
{
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i || i->op != ssa::IrInstr::Op::Compare || i->uses.size() < 2) continue;
			// `cmp %sil, (%rax)` reads one byte straight from memory.
			auto byteOperand = [&](ssa::ValueId id) -> const ssa::IrInstr* {
				if (const auto* m = fn.value(id); m && m->kind == ssa::ValueKind::MemRef && m->memWidth == 1)
					return i;
				return byteLoadOf(fn, id);
			};
			const auto* a = byteOperand(i->uses[0].valueId);
			const auto* b = byteOperand(i->uses[1].valueId);
			if (a && b && a != b) return true;
		}
	}
	return false;
}

// ─── Arithmetic loop idioms ───────────────────────────────────────────────────
// Each looks only at instructions in blocks on a cycle, and at operands, so a
// constant elsewhere in the function is not evidence.

bool immOperand(const ssa::SSAFunction& fn, const ssa::IrInstr& i, uint64_t value)
{
	for (const auto& u: i.uses)
		if (const auto* v = fn.value(u.valueId); v && v->kind == ssa::ValueKind::Immediate && v->imm == value)
			return true;
	return false;
}

bool anyImmOperand(const ssa::SSAFunction& fn, const ssa::IrInstr& i)
{
	for (const auto& u: i.uses)
		if (const auto* v = fn.value(u.valueId); v && v->kind == ssa::ValueKind::Immediate) return true;
	return false;
}

bool addrOnStack(const ssa::SSAFunction& fn, const ssa::IrInstr& i)
{
	const uint32_t idx = i.op == ssa::IrInstr::Op::Store ? 1 : 0;
	for (const auto& u: i.uses)
		if (u.operandIndex == idx)
			if (const auto* a = fn.value(u.valueId); a && a->isStackSlot) return true;
	return false;
}

template <typename F>
void forEachLoopInstr(const ssa::SSAFunction& fn, F&& f)
{
	for (const auto& blk: fn.blocks())
	{
		if (!blk || !ssa::blockOnCycle(fn, blk->id)) continue;
		for (const auto* i: blk->instrs)
			if (i) f(*i);
	}
}

/// Every load reads a stack slot: the loop works on scalars, not on memory.
bool onlyScalarLoads(const ssa::SSAFunction& fn)
{
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == ssa::IrInstr::Op::Load && !addrOnStack(fn, *i)) return false;
	}
	return true;
}

bool hasDigitLoop(const ssa::SSAFunction& fn)
{
	// `c >= '0' && c <= '9'`, which gcc may also test as `c > '/'`, or
	// folded into one unsigned range check `(c - '0') < 10`, where the
	// subtraction is an add of -'0' at the byte, word or register width.
	// Only instructions inside the loop count.
	bool zero = false, nine = false, minusZero = false, ten = false, scale = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if (i.op == ssa::IrInstr::Op::Compare) {
			zero = zero || immOperand(fn, i, 48u) || immOperand(fn, i, 47u);
			nine = nine || immOperand(fn, i, 57u) || immOperand(fn, i, 58u);
			ten = ten || immOperand(fn, i, 10u) || immOperand(fn, i, 9u);
		}
		if (i.op == ssa::IrInstr::Op::Add || i.op == ssa::IrInstr::Op::Sub) {
			minusZero = minusZero || immOperand(fn, i, 0xD0u) || immOperand(fn, i, 0xFFD0u)
				|| immOperand(fn, i, 0xFFFFFFD0u) || immOperand(fn, i, static_cast<uint64_t>(-48))
				|| (i.op == ssa::IrInstr::Op::Sub && immOperand(fn, i, 48u));
		}
		if (i.op == ssa::IrInstr::Op::Mul || i.op == ssa::IrInstr::Op::Shl) scale = true;
	});
	return ((zero && nine) || (minusZero && ten)) && scale;
}

/// A reflected or normal CRC-32 polynomial XORed in inside a shifting loop.
bool hasCrcLoop(const ssa::SSAFunction& fn)
{
	bool poly = false, shift = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if ((immOperand(fn, i, 0xEDB88320u) || immOperand(fn, i, 0x04C11DB7u) || immOperand(fn, i, 0x82F63B78u)))
			poly = true;
		if (((i.op == ssa::IrInstr::Op::Shr || i.op == ssa::IrInstr::Op::Shl) && immOperand(fn, i, 1))
			|| (i.op == ssa::IrInstr::Op::Div && immOperand(fn, i, 2)))
			shift = true;
	});
	return poly && shift;
}

/// `c += x & 1; x >>= 1;` over a scalar: the low bit counted, then shifted out.
bool hasBitCountLoop(const ssa::SSAFunction& fn)
{
	bool lowBit = false, shiftOne = false, add = false, xorIn = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if ((i.op == ssa::IrInstr::Op::And && immOperand(fn, i, 1)) || (i.op == ssa::IrInstr::Op::Rem && immOperand(fn, i, 2)))
			lowBit = true;
		if (((i.op == ssa::IrInstr::Op::Shr || i.op == ssa::IrInstr::Op::Sar) && immOperand(fn, i, 1))
			|| (i.op == ssa::IrInstr::Op::Div && immOperand(fn, i, 2)))
			shiftOne = true;
		if (i.op == ssa::IrInstr::Op::Add) add = true;
		if (i.op == ssa::IrInstr::Op::Xor) xorIn = true;
	});
	return lowBit && shiftOne && add && !xorIn && onlyScalarLoads(fn);
}

/// Euclid: a remainder by a variable inside a loop over scalars.
bool hasEuclidLoop(const ssa::SSAFunction& fn)
{
	bool remByVar = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if (i.op == ssa::IrInstr::Op::Rem && !anyImmOperand(fn, i)) remByVar = true;
	});
	return remByVar && onlyScalarLoads(fn) && !hasCrcLoop(fn);
}

/// `buf[i] ^= k`: a byte loaded from memory, XORed, and stored back to memory.
bool hasXorBufferLoop(const ssa::SSAFunction& fn)
{
	bool found = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& st) {
		if (found || st.op != ssa::IrInstr::Op::Store || st.uses.empty() || addrOnStack(fn, st)) return;
		const auto* v = fn.value(st.uses[0].valueId);
		const ssa::IrInstr* d = v ? v->defInstr : nullptr;
		for (int hops = 0; d && d->op == ssa::IrInstr::Op::Assign && !d->uses.empty() && hops < 2; ++hops)
		{
			v = fn.value(d->uses[0].valueId);
			d = v ? v->defInstr : nullptr;
		}
		if (!d || d->op != ssa::IrInstr::Op::Xor) return;
		for (const auto& u: d->uses)
			if (const auto* ld = byteLoadOf(fn, u.valueId); ld && !addrOnStack(fn, *ld)) found = true;
	});
	return found && !hasCrcLoop(fn);
}

/// `p[i] = v`: a store to memory of a value not read from memory, in a
/// loop that reads no memory at all.
bool hasByteFillLoop(const ssa::SSAFunction& fn)
{
	if (!onlyScalarLoads(fn) || countOp(fn, ssa::IrInstr::Op::Call) > 0) return false;
	bool fill = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& st) {
		if (st.op != ssa::IrInstr::Op::Store || st.uses.empty() || addrOnStack(fn, st)) return;
		// The fill value is passed through, not computed: a value built by
		// arithmetic each iteration is an encoder, not a fill.
		const auto* v = fn.value(st.uses[0].valueId);
		const ssa::IrInstr* d = v ? v->defInstr : nullptr;
		for (int hops = 0; d && d->op == ssa::IrInstr::Op::Assign && !d->uses.empty() && hops < 3; ++hops)
		{
			v = fn.value(d->uses[0].valueId);
			d = v ? v->defInstr : nullptr;
		}
		if (!d || d->op == ssa::IrInstr::Op::Load) fill = true;
	});
	return fill;
}

/// A loop that writes to memory other than its own stack slots: it builds
/// something (a table, an output buffer), which strlen and strcmp never do.
bool writesMemoryInLoop(const ssa::SSAFunction& fn)
{
	bool writes = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if (i.op == ssa::IrInstr::Op::Store && i.uses.size() >= 2 && !addrOnStack(fn, i)) writes = true;
	});
	return writes;
}

/// The value stored by a Store, followed through a few register copies to
/// the instruction that computed it.
const ssa::IrInstr* storedValueDef(const ssa::SSAFunction& fn, const ssa::IrInstr& st)
{
	if (st.uses.empty()) return nullptr;
	const auto* v = fn.value(st.uses[0].valueId);
	const ssa::IrInstr* d = v ? v->defInstr : nullptr;
	for (int hops = 0; d && d->op == ssa::IrInstr::Op::Assign && !d->uses.empty() && hops < 3; ++hops)
	{
		v = fn.value(d->uses[0].valueId);
		d = v ? v->defInstr : nullptr;
	}
	return d;
}

bool isPow2(uint64_t v) { return v >= 2 && (v & (v - 1)) == 0; }

/// Operand `idx` of `i` is the immediate `value`.
bool immAt(const ssa::SSAFunction& fn, const ssa::IrInstr& i, uint32_t idx, uint64_t value)
{
	for (const auto& u: i.uses)
		if (const auto* v = fn.value(u.valueId); u.operandIndex == idx && v && v->kind == ssa::ValueKind::Immediate && v->imm == value)
			return true;
	return false;
}

/// `h = h * K + c` over a string, then `bits |= 1 << (h % M)` or
/// `(bits >> (h % M)) & 1`: a multiplicative string hash in a loop, reduced
/// modulo a power of two outside it and used as a bit position.
bool hasBloomFilterBit(const ssa::SSAFunction& fn)
{
	bool hashMul = false, byteLoad = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		if (i.op == ssa::IrInstr::Op::Mul)
			for (const auto& u: i.uses)
				if (const auto* v = fn.value(u.valueId); v && v->kind == ssa::ValueKind::Immediate && v->imm > 8 && (v->imm & 1))
					hashMul = true;
		if (i.op == ssa::IrInstr::Op::Load && !addrOnStack(fn, i)) byteLoad = true;
	});
	if (!hashMul || !byteLoad) return false;
	bool bitIndex = false, bitOp = false;
	for (const auto& blk: fn.blocks())
	{
		if (!blk || ssa::blockOnCycle(fn, blk->id)) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i) continue;
			if (i->op == ssa::IrInstr::Op::Rem)
				for (const auto& u: i->uses)
					if (const auto* v = fn.value(u.valueId); v && v->kind == ssa::ValueKind::Immediate && isPow2(v->imm) && v->imm <= 4096)
						bitIndex = true;
			// `1 << n` sets one bit; `x >> n` followed by `& 1` / `% 2` tests one.
			if (i->op == ssa::IrInstr::Op::Shl && immAt(fn, *i, 0, 1)) bitOp = true;
			if ((i->op == ssa::IrInstr::Op::Rem && immOperand(fn, *i, 2)) || (i->op == ssa::IrInstr::Op::And && immOperand(fn, *i, 1)))
				for (const auto& u: i->uses)
					if (const auto* v = fn.value(u.valueId); v && v->defInstr && v->defInstr->op == ssa::IrInstr::Op::Shr)
						bitOp = true;
		}
	}
	return bitIndex && bitOp;
}

/// Whether `id` is the result of a load from memory other than the stack,
/// looking through a few register copies.
bool fromMemoryLoad(const ssa::SSAFunction& fn, ssa::ValueId id)
{
	const auto* v = fn.value(id);
	const ssa::IrInstr* d = v ? v->defInstr : nullptr;
	for (int hops = 0; d && d->op == ssa::IrInstr::Op::Assign && !d->uses.empty() && hops < 3; ++hops)
	{
		v = fn.value(d->uses[0].valueId);
		d = v ? v->defInstr : nullptr;
	}
	return d && d->op == ssa::IrInstr::Op::Load && !addrOnStack(fn, *d);
}

/// `while (lo < hi) { mid = lo + (hi - lo) / 2; if (a[mid] < x) lo = mid + 1; else hi = mid; }`:
/// a halving loop that reads memory, writes none, and compares the element it
/// reads exactly once. A plain binary search also tests the element for
/// equality, which is a second compare. Returns the number of element
/// compares in such a loop, 0 if the loop is not a halving search.
int halvingSearchCompares(const ssa::SSAFunction& fn)
{
	bool halving = false, memStore = false, call = false;
	int elementCompares = 0;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		switch (i.op)
		{
		case ssa::IrInstr::Op::Shr: case ssa::IrInstr::Op::Sar:
			if (immOperand(fn, i, 1)) halving = true;
			break;
		case ssa::IrInstr::Op::Div:
			if (immOperand(fn, i, 2)) halving = true;
			break;
		case ssa::IrInstr::Op::Store:
			if (!addrOnStack(fn, i)) memStore = true;
			break;
		case ssa::IrInstr::Op::Call:
			call = true;
			break;
		case ssa::IrInstr::Op::Compare:
			for (const auto& u: i.uses)
				if (fromMemoryLoad(fn, u.valueId)) { ++elementCompares; break; }
			break;
		default:
			break;
		}
	});
	return halving && !memStore && !call ? elementCompares : 0;
}

/// `for (i = 0; i < n; ++i) if (a[i] == x) return i;`: a loop that reads
/// one element per iteration, writes no memory, calls nothing, and compares
/// the element with something other than zero (zero is a terminator scan
/// such as strlen) besides testing its own bound.
bool hasLinearSearchLoop(const ssa::SSAFunction& fn)
{
	int memLoads = 0, compares = 0;
	bool memStore = false, call = false, elementVsValue = false;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		switch (i.op)
		{
		case ssa::IrInstr::Op::Load:
			if (!addrOnStack(fn, i)) ++memLoads;
			break;
		case ssa::IrInstr::Op::Store:
			if (!addrOnStack(fn, i)) memStore = true;
			break;
		case ssa::IrInstr::Op::Call:
			call = true;
			break;
		case ssa::IrInstr::Op::Compare:
		{
			++compares;
			bool elem = false, zero = false;
			for (const auto& u: i.uses)
			{
				if (fromMemoryLoad(fn, u.valueId)) elem = true;
				if (const auto* v = fn.value(u.valueId); v && v->kind == ssa::ValueKind::Immediate && v->imm == 0) zero = true;
			}
			if (elem && !zero) elementVsValue = true;
			break;
		}
		default:
			break;
		}
	});
	return memLoads == 1 && !memStore && !call && compares >= 2 && elementVsValue;
}

/// `out[o++] = c; out[o++] = '0' + run;` after counting a run of equal
/// bytes: a loop that adds '0' to a count, writes at least two bytes out,
/// and compares a byte it read with another value. Turning a number into
/// decimal digits (`'0' + n % 10`) also adds '0', so a divide, remainder or
/// multiply by ten rules it out.
bool hasRleLoop(const ssa::SSAFunction& fn)
{
	bool addZero = false, decimal = false, byteVsByte = false;
	int memStores = 0;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		switch (i.op)
		{
		case ssa::IrInstr::Op::Add:
			if (immOperand(fn, i, 0x30)) addZero = true;
			break;
		case ssa::IrInstr::Op::Div: case ssa::IrInstr::Op::Rem: case ssa::IrInstr::Op::Mul:
			if (immOperand(fn, i, 10)) decimal = true;
			break;
		case ssa::IrInstr::Op::Store:
			if (!addrOnStack(fn, i)) ++memStores;
			break;
		case ssa::IrInstr::Op::Compare:
		{
			bool elem = false, imm = false;
			for (const auto& u: i.uses)
			{
				if (fromMemoryLoad(fn, u.valueId)) elem = true;
				if (const auto* v = fn.value(u.valueId); v && v->kind == ssa::ValueKind::Immediate) imm = true;
			}
			if (elem && !imm) byteVsByte = true;
			break;
		}
		default:
			break;
		}
	});
	return addZero && !decimal && byteVsByte && memStores >= 2;
}

/// What a loop does with array elements and floating-point values; the
/// numeric kernels below are told apart by these counts.
struct NumericLoopShape
{
	bool elementProduct = false; ///< `a[i] * b[j]`: a multiply of two loaded elements
	bool elementAccumulate = false; ///< `s += v[i]`: an add of a loaded element
	bool squareOfDifference = false; ///< `(v[i] - m) * (v[i] - m)`
	bool memStore = false;
	bool call = false;
	bool varFloatDiv = false; ///< a floating-point divide by a variable
	bool halfOrTwo = false; ///< float `* 0.5` or `/ 2.0`
	bool floatCompare = false;
	int condBranches = 0;
};

NumericLoopShape numericLoopShape(const ssa::SSAFunction& fn)
{
	NumericLoopShape sh;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		switch (i.op)
		{
		case ssa::IrInstr::Op::Mul:
			if (i.uses.size() >= 2 && fromMemoryLoad(fn, i.uses[0].valueId) && fromMemoryLoad(fn, i.uses[1].valueId)
				&& i.uses[0].valueId != i.uses[1].valueId)
				sh.elementProduct = true;
			if (i.uses.size() >= 2 && i.uses[0].valueId == i.uses[1].valueId)
				if (const auto* v = fn.value(i.uses[0].valueId); v && v->defInstr && v->defInstr->op == ssa::IrInstr::Op::Sub)
					for (const auto& u: v->defInstr->uses)
						if (fromMemoryLoad(fn, u.valueId)) sh.squareOfDifference = true;
			if (i.isFloat && immOperand(fn, i, 0x3FE0000000000000ULL)) sh.halfOrTwo = true;
			break;
		case ssa::IrInstr::Op::Div:
			if (i.isFloat && immAt(fn, i, 1, 0x4000000000000000ULL)) sh.halfOrTwo = true;
			// `a / x`: the divisor varies; the dividend may be a constant.
			else if (i.isFloat && [&] {
						 for (const auto& u: i.uses)
							 if (const auto* v = fn.value(u.valueId); u.operandIndex == 1 && v && v->kind == ssa::ValueKind::Immediate)
								 return false;
						 return true;
					 }())
				sh.varFloatDiv = true;
			break;
		case ssa::IrInstr::Op::Add:
			for (const auto& u: i.uses)
				if (fromMemoryLoad(fn, u.valueId)) sh.elementAccumulate = true;
			break;
		case ssa::IrInstr::Op::Store:
			if (!addrOnStack(fn, i)) sh.memStore = true;
			break;
		case ssa::IrInstr::Op::Call:
			sh.call = true;
			break;
		case ssa::IrInstr::Op::Compare:
			if (i.isFloat) sh.floatCompare = true;
			break;
		case ssa::IrInstr::Op::CondBranch:
			++sh.condBranches;
			break;
		default:
			break;
		}
	});
	return sh;
}

/// A floating-point divide outside every loop: the `/ n` that turns a sum
/// into a mean (`/ 5.0` once n is a known constant).
bool floatDivOutsideLoops(const ssa::SSAFunction& fn)
{
	for (const auto& blk: fn.blocks())
		if (blk && !ssa::blockOnCycle(fn, blk->id))
			for (const auto* i: blk->instrs)
				if (i && i->op == ssa::IrInstr::Op::Div && i->isFloat) return true;
	return false;
}

/// `t = a + b; a = b; b = t;`: a loop over scalars only whose body adds two
/// variables and shifts one of them into the other unchanged. Nothing else
/// is computed: no multiply, divide, shift or bitwise operation, no memory
/// beyond the function's own stack slots and no calls.
bool hasFibonacciLoop(const ssa::SSAFunction& fn)
{
	bool other = false, addVars = false, rotate = false;
	int slotStores = 0;
	forEachLoopInstr(fn, [&](const ssa::IrInstr& i) {
		switch (i.op)
		{
		case ssa::IrInstr::Op::Rem:
			// `% 2^32` is how a 32-bit wrap shows up after lifting; anything
			// else is real arithmetic.
			if (!immOperand(fn, i, 0x100000000ULL)) other = true;
			break;
		case ssa::IrInstr::Op::Mul: case ssa::IrInstr::Op::Div:
		case ssa::IrInstr::Op::Shl: case ssa::IrInstr::Op::Shr: case ssa::IrInstr::Op::Sar:
		case ssa::IrInstr::Op::And: case ssa::IrInstr::Op::Or: case ssa::IrInstr::Op::Xor:
		case ssa::IrInstr::Op::Call: case ssa::IrInstr::Op::Lock:
			other = true;
			break;
		case ssa::IrInstr::Op::Load:
			if (!addrOnStack(fn, i)) other = true;
			break;
		case ssa::IrInstr::Op::Add:
			if (i.uses.size() >= 2 && !anyImmOperand(fn, i)) addVars = true;
			break;
		case ssa::IrInstr::Op::Store:
			if (!addrOnStack(fn, i)) { other = true; break; }
			++slotStores;
			if (const auto* d = storedValueDef(fn, i); d && d->op == ssa::IrInstr::Op::Load && addrOnStack(fn, *d))
				rotate = true;
			break;
		default:
			break;
		}
	});
	return !other && addVars && rotate && slotStores >= 3;
}

// Blocks reachable from `from` without entering `stop`.
std::vector<uint8_t> reachWithout(const ssa::SSAFunction& fn, ssa::BlockId from, ssa::BlockId stop)
{
	std::vector<uint8_t> seen(fn.blocks().size(), 0);
	std::vector<ssa::BlockId> work{from};
	while (!work.empty())
	{
		const ssa::BlockId cur = work.back();
		work.pop_back();
		if (cur >= seen.size() || seen[cur]) continue;
		seen[cur] = 1;
		if (const auto* cb = fn.block(cur))
			for (ssa::BlockId s: cb->succs)
				if (s != stop) work.push_back(s);
	}
	return seen;
}

// A string walk that only walks: some loop whose body calls nothing.  A loop
// that tests each byte for NUL but hands it to a function -- `for (i = 0;
// msg[i]; ++i) push(msg[i]);` -- consumes the string; it does not measure it.
bool hasCallFreeLoop(const ssa::SSAFunction& fn)
{
	for (const auto& [tail, header]: ssa::retreatingEdges(fn))
	{
		const auto fromHeader = reachWithout(fn, header, header);
		bool calls = false;
		for (ssa::BlockId b = 0; b < fromHeader.size() && !calls; ++b)
		{
			if (!fromHeader[b] || !reachWithout(fn, b, header)[tail]) continue;
			if (const auto* blk = fn.block(b))
				for (const auto* i: blk->instrs)
					if (i && i->op == ssa::IrInstr::Op::Call) calls = true;
		}
		if (!calls) return true;
	}
	return false;
}

bool hasNullTerminatedLoop(const ssa::SSAFunction& fn)
{
	return hasBackEdge(fn) && hasByteTestedAgainstZero(fn) && countOp(fn, ssa::IrInstr::Op::Mul) == 0
		&& hasCallFreeLoop(fn);
}

// A graph DFS recurses from inside the loop over a node's neighbours. Any
// recursive function has a self-call, a store and loads -- quicksort recurses
// twice in straight line and heapify once, and both were reported as DFS.
bool hasSelfCallInLoop(const ssa::SSAFunction& fn)
{
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == ssa::IrInstr::Op::Call && i->calleeName == fn.name() && ssa::blockOnCycle(fn, blk->id))
				return true;
	}
	return false;
}

bool hasDfsStructure(const ssa::SSAFunction& fn)
{
	// A traversal marks nodes; it does not move elements.  Quicksort at -O2
	// turns its second recursion into a loop around the first, putting a
	// self-call inside a loop that swaps.
	return hasSelfCallInLoop(fn) && countOp(fn, ssa::IrInstr::Op::Store) >= 1 && countOp(fn, ssa::IrInstr::Op::Load) >= 2
		&& ssa::countElementMoves(fn) == 0;
}

IdiomResult makeResult(IdiomKind kind, float confidence, const std::string& detail)
{
	IdiomResult r;
	r.kind = kind;
	r.confidence = confidence;
	r.detail = detail;
	return r;
}

} // anonymous namespace

std::string IdiomResult::primaryLabel() const noexcept
{
	switch (kind)
	{
	case IdiomKind::Atoi: return "Atoi";
	case IdiomKind::Strlen: return "Strlen";
	case IdiomKind::Strcmp: return "Strcmp";
	case IdiomKind::Bfs: return "BFS";
	case IdiomKind::Dfs: return "DFS";
	case IdiomKind::Varint: return "Varint";
	case IdiomKind::Gcd: return "GCD";
	case IdiomKind::Crc: return "CRC";
	// A6: Knapsack / Fibonacci / LCS are enum labels only — detect() never
	// assigns them (no generalisable structural signature).
	case IdiomKind::Knapsack: return "Knapsack";
	case IdiomKind::Rle: return "RLE";
	case IdiomKind::Fibonacci: return "Fibonacci";
	case IdiomKind::Lcs: return "LCS";
	case IdiomKind::Memset: return "Memset";
	case IdiomKind::Popcount: return "Popcount";
	case IdiomKind::BloomFilter: return "BloomFilter";
	case IdiomKind::MatrixMultiply: return "MatrixMultiply";
	case IdiomKind::LinkedList: return "LinkedList";
	case IdiomKind::XorCipher: return "XOR";
	case IdiomKind::LowerBound: return "LowerBound";
	case IdiomKind::LinearSearch: return "LinearSearch";
	case IdiomKind::BinarySearch: return "BinarySearch";
	case IdiomKind::Stack: return "Stack";
	case IdiomKind::Queue: return "Queue";
	case IdiomKind::ShellSort: return "ShellSort";
	case IdiomKind::MemcpyLoop: return "Memcpy";
	case IdiomKind::RingBuffer: return "RingBuffer";
	case IdiomKind::HashTableChaining: return "HashTable";
	case IdiomKind::DotProduct: return "DotProduct";
	case IdiomKind::Statistics: return "Statistics";
	case IdiomKind::NewtonRaphson: return "NewtonRaphson";
	default: return "Unknown";
	}
}

std::vector<std::string> IdiomResult::exportLabels() const
{
	switch (kind)
	{
	case IdiomKind::Atoi: return {"Atoi", "Parse"};
	case IdiomKind::Strlen: return {"Strlen", "String"};
	case IdiomKind::Strcmp: return {"Strcmp", "String"};
	case IdiomKind::Bfs: return {"BFS", "GraphTraversal"};
	case IdiomKind::Dfs: return {"DFS", "GraphTraversal"};
	case IdiomKind::Varint: return {"Varint", "Serialization"};
	case IdiomKind::Gcd: return {"GCD", "Euclid"};
	case IdiomKind::Crc: return {"CRC", "Checksum"};
	case IdiomKind::Knapsack: return {"Knapsack", "DynamicProgramming"};
	case IdiomKind::Rle: return {"RLE", "Compression"};
	case IdiomKind::Fibonacci: return {"Fibonacci", "DynamicProgramming"};
	case IdiomKind::Lcs: return {"LCS", "DynamicProgramming"};
	case IdiomKind::Memset: return {"Memset", "Memory"};
	case IdiomKind::Popcount: return {"Popcount", "BitManipulation"};
	case IdiomKind::BloomFilter: return {"BloomFilter", "Probabilistic"};
	// Not "LinearAlgebra": the corpus gives that to the integer kernel only,
	// and nothing in the loop says which of the two it is looking at.
	case IdiomKind::MatrixMultiply: return {"MatrixMultiply"};
	case IdiomKind::LinkedList: return {"LinkedList"};
	case IdiomKind::XorCipher: return {"XOR", "Cipher"};
	case IdiomKind::LowerBound: return {"LowerBound", "BinarySearch", "Search"};
	case IdiomKind::LinearSearch: return {"LinearSearch", "Search"};
	case IdiomKind::BinarySearch: return {"BinarySearch", "Search"};
	case IdiomKind::Stack: return {"Stack", "LIFO"};
	case IdiomKind::Queue: return {"Queue", "FIFO"};
	case IdiomKind::ShellSort: return {"ShellSort", "Sort"};
	case IdiomKind::MemcpyLoop: return {"Memcpy", "Copy", "Memmove"};
	case IdiomKind::RingBuffer: return {"RingBuffer", "CircularBuffer"};
	case IdiomKind::HashTableChaining: return {"HashTable", "Chaining"};
	case IdiomKind::DotProduct: return {"DotProduct"};
	case IdiomKind::Statistics: return {"Statistics"};
	case IdiomKind::NewtonRaphson: return {"NewtonRaphson"};
	default: return {};
	}
}

std::string IdiomResult::toString() const
{
	std::ostringstream os;
	os << primaryLabel() << " (confidence=" << confidence << ")";
	if (!detail.empty()) os << " " << detail;
	return os.str();
}

IdiomDetector::IdiomDetector(Config cfg): cfg_(std::move(cfg)) {}

bool IdiomDetector::passesPreflight(const ssa::SSAFunction& fn) const
{
	if ((int)fn.blockCount() < cfg_.minBlocks) return false;
	int instrs = 0;
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		instrs += static_cast<int>(blk->instrs.size());
	}
	return instrs >= cfg_.minInstrs;
}

std::vector<IdiomResult> IdiomDetector::detect(const ssa::SSAFunction& fn) const
{
	std::vector<IdiomResult> out;
	if (!passesPreflight(fn)) return out;

	if (hasDigitLoop(fn))
	{
		out.push_back(makeResult(IdiomKind::Atoi, 0.88f, "digit_loop"));
	}

	if (hasNullTerminatedLoop(fn) && !writesMemoryInLoop(fn))
	{
		if (hasBytePairCompare(fn))
			out.push_back(makeResult(IdiomKind::Strcmp, 0.82f, "null_term_byte_pair_compare"));
		else
			out.push_back(makeResult(IdiomKind::Strlen, 0.82f, "null_term_loop"));
	}

	if (hasDfsStructure(fn))
	{
		out.push_back(makeResult(IdiomKind::Dfs, 0.90f, "recursive_adjacency"));
	}

	if (hasCrcLoop(fn)) out.push_back(makeResult(IdiomKind::Crc, 0.90f, "crc_polynomial_shift_loop"));
	else if (hasBitCountLoop(fn)) out.push_back(makeResult(IdiomKind::Popcount, 0.80f, "low_bit_shift_loop"));
	if (hasEuclidLoop(fn)) out.push_back(makeResult(IdiomKind::Gcd, 0.80f, "remainder_by_variable_loop"));
	if (hasXorBufferLoop(fn)) out.push_back(makeResult(IdiomKind::XorCipher, 0.80f, "xor_byte_in_place"));
	if (hasByteFillLoop(fn)) out.push_back(makeResult(IdiomKind::Memset, 0.75f, "byte_fill_loop"));

	if (hasVarintLoop(fn))
	{
		out.push_back(makeResult(IdiomKind::Varint, 0.92f, "shift7_mask7f"));
	}

	if (const int cmps = halvingSearchCompares(fn); cmps == 1)
		out.push_back(makeResult(IdiomKind::LowerBound, 0.75f, "halving_loop_one_element_compare"));
	else if (cmps == 2)
		out.push_back(makeResult(IdiomKind::BinarySearch, 0.75f, "halving_loop_equal_and_less_compares"));
	if (hasBloomFilterBit(fn)) out.push_back(makeResult(IdiomKind::BloomFilter, 0.75f, "string_hash_mod_pow2_bit"));
	if (hasLinearSearchLoop(fn) && !halvingSearchCompares(fn))
		out.push_back(makeResult(IdiomKind::LinearSearch, 0.75f, "one_element_compared_per_iteration"));
	if (hasRleLoop(fn)) out.push_back(makeResult(IdiomKind::Rle, 0.75f, "run_count_written_as_digit"));
	{
		const NumericLoopShape sh = numericLoopShape(fn);
		// `s += a[i] * b[i]` in a single loop that writes nothing back.
		if (sh.elementProduct && !sh.memStore && !sh.call && sh.condBranches == 1)
			out.push_back(makeResult(IdiomKind::DotProduct, 0.75f, "sum_of_element_products"));
		// The same product inside three nested loops, written to the result.
		else if (sh.elementProduct && sh.memStore && !sh.call && sh.condBranches >= 3)
			out.push_back(makeResult(IdiomKind::MatrixMultiply, 0.75f, "element_products_in_three_loops"));
		// `sum(v[i]) / n` or `sum((v[i] - m)^2) / n`.
		else if ((sh.elementAccumulate || sh.squareOfDifference) && !sh.elementProduct && !sh.memStore && !sh.call
				 && sh.condBranches <= 3 && floatDivOutsideLoops(fn))
			out.push_back(makeResult(IdiomKind::Statistics, 0.75f, "sum_divided_by_count"));
		// `x = 0.5 * (x + a / x)` until it settles.
		if (sh.varFloatDiv && sh.halfOrTwo && sh.floatCompare && !sh.memStore)
			out.push_back(makeResult(IdiomKind::NewtonRaphson, 0.75f, "halved_sum_with_quotient"));
	}
	if (hasFibonacciLoop(fn)) out.push_back(makeResult(IdiomKind::Fibonacci, 0.80f, "add_and_rotate_scalars"));
	// A6: do not assign Knapsack or Lcs.

	out.erase(
		std::remove_if(
			out.begin(), out.end(), [this](const IdiomResult& r) { return r.confidence < cfg_.minConfidence; }),
		out.end());
	return out;
}

} // namespace algo_recover
} // namespace retdec
