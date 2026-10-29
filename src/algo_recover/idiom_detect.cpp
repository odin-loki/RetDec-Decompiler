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

bool hasVarintLoop(const ssa::SSAFunction& fn)
{
	const bool mask7f = hasConstant(fn, 0x7Fu);
	const bool mask80 = hasConstant(fn, 0x80u);
	const bool shr7 = hasConstant(fn, 7u) && countOp(fn, ssa::IrInstr::Op::Shr) >= 1;
	return hasBackEdge(fn) && mask7f && shr7 && (mask80 || countOp(fn, ssa::IrInstr::Op::And) >= 1);
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
	case IdiomKind::MatrixMultiply: return {"MatrixMultiply", "LinearAlgebra"};
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

	// A6: do not assign Knapsack, Fibonacci, or Lcs.

	out.erase(
		std::remove_if(
			out.begin(), out.end(), [this](const IdiomResult& r) { return r.confidence < cfg_.minConfidence; }),
		out.end());
	return out;
}

} // namespace algo_recover
} // namespace retdec
