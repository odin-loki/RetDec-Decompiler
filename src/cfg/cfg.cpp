/**
 * @file src/cfg/cfg.cpp
 * @brief Typed-edge CFG construction with jump table and vtable resolution.
 *
 * ## Phase 1 — Basic block construction + direct edges
 *
 * For each function, iterate the instruction sequence:
 *   - Normal instructions extend the current basic block.
 *   - Any control-flow instruction terminates the current block and emits edges:
 *       DirectJmp   → FallThrough (if conditional) + TrueBranch (always)
 *       ConditionalJmp → TrueBranch to target + FalseBranch to next instruction
 *       DirectCall  → DirectCall edge; FallThrough edge to next instruction
 *       Ret         → no outgoing edges (exit block)
 *       TailCall    → TailCall edge
 *       IndirectJmp → placeholder UnresolvedIndirect edge (resolved in Phase 2)
 *       IndirectCall→ placeholder UnresolvedIndirect edge
 *
 * Block splits: if a branch target lands in the middle of an existing block,
 * the existing block is split at that address and edges rewired.
 *
 * ## Phase 2 — Indirect resolution
 *
 * ### (a) Exception handlers
 *   If addExceptionHandler(instrAddr, handlerAddr) was called for an indirect
 *   JMP address, replace the UnresolvedIndirect edge with an ExceptionEdge.
 *
 * ### (b) Jump tables
 *   Walk the pre-registered JumpTableInfo list.  For each entry, call
 *   resolveJumpTable() to read the target array from the raw image:
 *     GCC/Clang: targets[i] = read64(tableBase + i*stride)
 *     MSVC:      targets[i] = tableBase + (int32)read32(tableBase + i*4)
 *   Add one SwitchEdge per target.  Remove the UnresolvedIndirect placeholder.
 *
 *   Automatic detection (VSA-lite):
 *   For any remaining UnresolvedIndirect JMP, scan the preceding 32 bytes for
 *   a CMP reg, imm + JA/JAE pattern.  If found, extract the bound and guess
 *   the table base from the MOV/LEA instruction immediately before the JMP.
 *
 * ### (c) Virtual calls
 *   For each vtable, enumerate its slots; for each block that ends in an
 *   UnresolvedIndirect CALL, emit one VirtualCallEdge per slot.
 *
 * ### (d) Remaining
 *   Any remaining UnresolvedIndirect edges get a diagnostic message.
 *
 * ## Phase 3 — Back-edge detection
 *
 * DFS from each function entry using grey (in-stack) / black (done) colouring.
 * An edge (u → v) where v is grey is a back edge → reclassify as LoopBackEdge.
 */

#include "retdec/cfg/cfg.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace retdec {
namespace cfg {

// ─── CFGGraph query helpers ───────────────────────────────────────────────────

const BasicBlock* CFGGraph::blockAt(uint64_t addr) const noexcept
{
	auto it = nodes.find(addr);
	return it != nodes.end() ? &it->second : nullptr;
}

std::vector<CFGEdge> CFGGraph::successorsOf(uint64_t blockAddr) const
{
	auto it = nodes.find(blockAddr);
	if (it == nodes.end()) return {};
	return it->second.succs;
}

std::vector<uint64_t> CFGGraph::predecessorsOf(uint64_t blockAddr) const
{
	auto it = nodes.find(blockAddr);
	if (it == nodes.end()) return {};
	return it->second.preds;
}

std::size_t CFGGraph::countEdges(EdgeType t) const noexcept
{
	std::size_t n = 0;
	for (const auto& [_, blk]: nodes)
		for (const auto& e: blk.succs)
			if (e.type == t) ++n;
	return n;
}

std::size_t CFGGraph::totalEdges() const noexcept
{
	std::size_t n = 0;
	for (const auto& [_, blk]: nodes)
		n += blk.succs.size();
	return n;
}

// ─── CFGBuilder constructor ───────────────────────────────────────────────────

CFGBuilder::CFGBuilder(uint64_t imageBase, const uint8_t* data, std::size_t size, bool is64Bit):
	_imageBase(imageBase), _data(data), _size(size), _is64Bit(is64Bit)
{}

// ─── Input registration ───────────────────────────────────────────────────────

void CFGBuilder::addFunction(uint64_t start, uint64_t end, const std::vector<InstrSummary>& instrs)
{
	_functions.push_back({start, end, instrs});
	_funcEntries.insert(start);
}

void CFGBuilder::addJumpTable(const JumpTableInfo& jt)
{
	_jumpTables.push_back(jt);
}

void CFGBuilder::addVtable(const VtableInfo& vt)
{
	_vtables.push_back(vt);
}

void CFGBuilder::addExceptionHandler(uint64_t instrAddr, uint64_t handlerAddr)
{
	_exHandlers[instrAddr] = handlerAddr;
}

// ─── Raw memory helpers ───────────────────────────────────────────────────────

std::size_t CFGBuilder::vaToOffset(uint64_t va) const noexcept
{
	if (va < _imageBase) return _size;
	uint64_t off = va - _imageBase;
	if (off >= _size) return _size;
	return static_cast<std::size_t>(off);
}

bool CFGBuilder::inBounds(uint64_t va, std::size_t sz) const noexcept
{
	std::size_t off = vaToOffset(va);
	return off < _size && sz <= _size - off;
}

int32_t CFGBuilder::readI32(uint64_t va) const noexcept
{
	std::size_t off = vaToOffset(va);
	if (off + 4 > _size) return 0;
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i)
		v |= static_cast<uint32_t>(_data[off + i]) << (i * 8);
	return static_cast<int32_t>(v);
}

uint32_t CFGBuilder::readU32(uint64_t va) const noexcept
{
	return static_cast<uint32_t>(readI32(va));
}

uint64_t CFGBuilder::readU64(uint64_t va) const noexcept
{
	std::size_t off = vaToOffset(va);
	if (off + 8 > _size) return 0;
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
		v |= static_cast<uint64_t>(_data[off + i]) << (i * 8);
	return v;
}

uint64_t CFGBuilder::readPtr(uint64_t va) const noexcept
{
	return _is64Bit ? readU64(va) : readU32(va);
}

// ─── Block helpers ────────────────────────────────────────────────────────────

BasicBlock& CFGBuilder::ensureBlock(uint64_t addr, uint64_t funcStart)
{
	auto it = _graph.nodes.find(addr);
	if (it != _graph.nodes.end()) return it->second;
	BasicBlock bb;
	bb.startAddr = addr;
	bb.endAddr = addr; // filled in during Phase 1
	bb.functionAddr = funcStart;
	_graph.nodes[addr] = std::move(bb);
	// _graph.nodes is a hash map, so "which block contains this address" has
	// no answer there short of a full scan. Blocks are only ever added, never
	// removed, so an ordered index of their starts costs one insert here and
	// turns that scan into a lower_bound.
	_blockStarts.insert(addr);
	return _graph.nodes[addr];
}

void CFGBuilder::addEdge(uint64_t from, uint64_t to, EdgeType type, uint32_t switchIdx)
{
	CFGEdge e;
	e.from = from;
	e.to = to;
	e.type = type;
	e.switchIndex = switchIdx;

	auto fit = _graph.nodes.find(from);
	if (fit != _graph.nodes.end())
	{
		fit->second.succs.push_back(e);
	}
	if (to != 0)
	{
		auto tit = _graph.nodes.find(to);
		if (tit != _graph.nodes.end())
		{
			tit->second.preds.push_back(from);
		}
	}
}

uint64_t CFGBuilder::blockStartContaining(uint64_t addr, uint64_t fallback) const
{
	// Blocks within a function do not overlap, so the containing block, if
	// there is one, is the one with the greatest start not above addr.
	auto it = _blockStarts.upper_bound(addr);
	if (it == _blockStarts.begin()) return fallback;
	--it;
	auto nit = _graph.nodes.find(*it);
	if (nit == _graph.nodes.end()) return fallback;
	const BasicBlock& blk = nit->second;
	if (blk.endAddr <= *it) return fallback;   // empty or unfinished
	if (addr >= blk.endAddr) return fallback;
	return *it;
}

void CFGBuilder::splitBlockAt(uint64_t splitAddr)
{
	// Both branch call sites do ensureBlock(target) immediately before calling
	// this, so a block starting exactly at splitAddr always exists by the time
	// we get here -- and the loop that used to be here returned the moment it
	// saw one, in whatever order the unordered_map happened to yield. With
	// libstdc++ the just-inserted key comes first, so the early return fired
	// every time: measured over 512 base addresses, a conditional jump back
	// into the middle of its own block produced a zero-length dead-end block
	// at the target and left the containing block unsplit, spanning it. Every
	// loop in every function came out that shape. And in the orders where the
	// containing block came first instead, the split overwrote the target
	// block wholesale -- `_graph.nodes[splitAddr] = std::move(newBlk)` --
	// discarding the predecessor addEdge had just recorded on it.
	//
	// So: find the containing block first, and split INTO whatever is already
	// at splitAddr rather than over it.
	// The containing block is the one with the greatest start below splitAddr,
	// found through the ordered index rather than by scanning every node --
	// this runs once per branch, and a scan made building a 20,000-block
	// function take 2.7 s where the index takes 16 ms.
	auto sit = _blockStarts.lower_bound(splitAddr);
	if (sit == _blockStarts.begin()) return;
	--sit;
	const uint64_t containerStart = *sit;
	auto cnit = _graph.nodes.find(containerStart);
	if (cnit == _graph.nodes.end()) return;
	{
		// endAddr == 0 marks a block Phase 1 has not finished; endAddr ==
		// startAddr marks one ensureBlock created and nothing has filled in.
		// Neither contains anything, so neither is splittable.
		const BasicBlock& c = cnit->second;
		if (c.endAddr == 0 || c.endAddr <= containerStart) return;
		if (splitAddr >= c.endAddr) return;
	}

	// The tail of the container becomes the block at splitAddr. Read what we
	// need out of the container before touching the map: a rehash does not
	// invalidate references, but a second lookup is cheaper to reason about.
	std::vector<CFGEdge> movedSuccs;
	uint64_t tailEnd = 0;
	uint64_t funcAddr = 0;
	{
		auto cit = _graph.nodes.find(containerStart);
		if (cit == _graph.nodes.end()) return;
		movedSuccs = std::move(cit->second.succs);
		cit->second.succs.clear();
		tailEnd = cit->second.endAddr;
		funcAddr = cit->second.functionAddr;
		cit->second.endAddr = splitAddr;
	}

	// The container's successors are now the tail's successors, so each of
	// them lists the tail rather than the container as a predecessor.
	for (const auto& e: movedSuccs)
	{
		if (e.to == 0) continue;
		auto sit = _graph.nodes.find(e.to);
		if (sit == _graph.nodes.end()) continue;
		for (auto& p: sit->second.preds)
		{
			if (p == containerStart)
			{
				p = splitAddr;
				break;
			}
		}
	}

	// Merge into the existing block if there is one: its preds are the
	// branches that made us split here in the first place.
	BasicBlock& tail = ensureBlock(splitAddr, funcAddr);
	tail.startAddr = splitAddr;
	tail.endAddr = tailEnd;
	tail.functionAddr = funcAddr;
	for (auto& e: movedSuccs) tail.succs.push_back(e);

	// Add fallthrough edge from the container to the tail.
	addEdge(containerStart, splitAddr, EdgeType::FallThrough);
}

// ─── Phase 1 ──────────────────────────────────────────────────────────────────

void CFGBuilder::buildBlocksForFunction(const FunctionInfo& fi)
{
	if (fi.instrs.empty())
	{
		ensureBlock(fi.start, fi.start).endAddr = fi.end;
		return;
	}

	uint64_t currentBlockStart = fi.start;
	ensureBlock(currentBlockStart, fi.start);

	for (std::size_t i = 0; i < fi.instrs.size(); ++i)
	{
		const InstrSummary& ins = fi.instrs[i];
		uint64_t nextAddr = ins.addr + ins.len;

		// Update end address of current block.
		{
			auto it = _graph.nodes.find(currentBlockStart);
			if (it != _graph.nodes.end())
			{
				it->second.endAddr = nextAddr;
			}
		}

		switch (ins.kind)
		{
		case InstrKind::Normal:
		case InstrKind::IndirectCall:
			// IndirectCall: emit an unresolved indirect edge but continue
			// the basic block (fallthrough to next instruction).
			if (ins.kind == InstrKind::IndirectCall)
			{
				addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
			}
			break;

		case InstrKind::DirectCall:
			// Emit DirectCall edge; block continues with fallthrough.
			if (ins.target != 0)
			{
				// Ensure target exists before addEdge so preds are recorded.
				ensureBlock(ins.target, ins.target);
				addEdge(currentBlockStart, ins.target, EdgeType::DirectCall);
			}
			// FallThrough to next instruction stays in same block — no split.
			break;

		case InstrKind::DirectJmp:
			if (ins.target != 0)
			{
				// Ensure target exists before addEdge so preds are recorded.
				ensureBlock(ins.target, fi.start);
				addEdge(currentBlockStart, ins.target, EdgeType::TrueBranch);
				// If target is mid-block, split.
				splitBlockAt(ins.target);
			}
			else
			{
				addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
			}
			// Start new block at next instruction if more instrs follow.
			// Use the actual next instruction address (not nextAddr) to handle
			// non-sequential instruction layouts (e.g. gaps between blocks).
			if (i + 1 < fi.instrs.size())
			{
				currentBlockStart = fi.instrs[i + 1].addr;
				ensureBlock(currentBlockStart, fi.start);
			}
			break;

		case InstrKind::ConditionalJmp:
			if (ins.target != 0)
			{
				// Ensure target exists before addEdge so preds are recorded.
				ensureBlock(ins.target, fi.start);
				addEdge(currentBlockStart, ins.target, EdgeType::TrueBranch);
				splitBlockAt(ins.target);
				// A backward branch into the current block splits it, and this
				// instruction is then in the tail, not in currentBlockStart any
				// more. The false-branch edge below has to leave the block the
				// jump is actually in -- otherwise it hangs off the half of the
				// block that ends before the jump. The true-branch edge above
				// is added first and moves with the split, so it needs nothing.
				currentBlockStart = blockStartContaining(ins.addr, currentBlockStart);
			}
			// Ensure fallthrough block exists before addEdge.
			ensureBlock(nextAddr, fi.start);
			addEdge(currentBlockStart, nextAddr, EdgeType::FalseBranch);
			// Start new block at fallthrough.
			currentBlockStart = nextAddr;
			break;

		case InstrKind::IndirectJmp:
			// Check if it's a known exception handler.
			{
				auto hit = _exHandlers.find(ins.addr);
				if (hit != _exHandlers.end())
				{
					addEdge(currentBlockStart, hit->second, EdgeType::ExceptionEdge);
				}
				else
				{
					addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
				}
			}
			if (i + 1 < fi.instrs.size())
			{
				currentBlockStart = fi.instrs[i + 1].addr;
				ensureBlock(currentBlockStart, fi.start);
			}
			break;

		case InstrKind::TailCall:
			if (ins.target != 0)
			{
				ensureBlock(ins.target, ins.target);
				addEdge(currentBlockStart, ins.target, EdgeType::TailCall);
			}
			else
			{
				addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
			}
			if (i + 1 < fi.instrs.size())
			{
				currentBlockStart = fi.instrs[i + 1].addr;
				ensureBlock(currentBlockStart, fi.start);
			}
			break;

		case InstrKind::Ret:
			// No outgoing CFG edge (function exit).
			// Use the actual next instruction's address to avoid creating
			// phantom blocks when there is a gap between this Ret and the
			// next instruction (e.g. separate basic blocks in the function).
			if (i + 1 < fi.instrs.size())
			{
				currentBlockStart = fi.instrs[i + 1].addr;
				ensureBlock(currentBlockStart, fi.start);
			}
			break;
		}
	}
}

void CFGBuilder::runPhase1()
{
	for (const auto& fi: _functions)
	{
		buildBlocksForFunction(fi);
	}
}

// ─── Phase 2a: exception edges ────────────────────────────────────────────────

void CFGBuilder::resolveExceptionEdges()
{
	for (auto& [addr, blk]: _graph.nodes)
	{
		for (auto& edge: blk.succs)
		{
			if (edge.type != EdgeType::UnresolvedIndirect) continue;
			// Check if this block's last indirect JMP has a registered handler.
			auto hit = _exHandlers.find(addr);
			if (hit != _exHandlers.end())
			{
				edge.type = EdgeType::ExceptionEdge;
				edge.to = hit->second;
				// Update pred list.
				auto tit = _graph.nodes.find(hit->second);
				if (tit != _graph.nodes.end())
				{
					tit->second.preds.push_back(addr);
				}
			}
		}
	}
}

// ─── Phase 2b: jump table resolution ─────────────────────────────────────────

std::vector<uint64_t> CFGBuilder::resolveJumpTable(const JumpTableInfo& jt) const
{
	std::vector<uint64_t> targets;
	uint64_t base = jt.tableBase;
	uint32_t stride = jt.stride ? jt.stride : ((_is64Bit && jt.fmt != JumpTableFmt::MSVC) ? 8u : 4u);

	for (uint32_t i = 0; i < jt.numEntries; ++i)
	{
		uint64_t entryVA = base + static_cast<uint64_t>(i) * stride;
		if (!inBounds(entryVA, stride)) break;

		uint64_t target = 0;
		switch (jt.fmt)
		{
		case JumpTableFmt::GCC:
		case JumpTableFmt::Clang: target = (stride == 8) ? readU64(entryVA) : readU32(entryVA); break;
		case JumpTableFmt::MSVC:
			// Signed 32-bit offset from table base.
			target = static_cast<uint64_t>(static_cast<int64_t>(base) + static_cast<int64_t>(readI32(entryVA)));
			break;
		}
		if (target == 0) break; // null terminator
		targets.push_back(target);
	}
	return targets;
}

uint32_t CFGBuilder::detectJumpTableBound(uint64_t jmpAddr) const noexcept
{
	// Scan backward up to 32 bytes from jmpAddr for CMP reg, imm + JA/JAE.
	// CMP r64, imm8: 48 83 F? <imm>
	// CMP r32, imm8: 83 F? <imm>
	// CMP r32, imm32: 81 F? <imm32>
	// JA  rel8: 77 <rel>   JA  rel32: 0F 87 <rel32>
	// JAE rel8: 73 <rel>   JAE rel32: 0F 83 <rel32>

	std::size_t jmpOff = vaToOffset(jmpAddr);
	if (jmpOff > 64)
		jmpOff -= 32;
	else
		jmpOff = 0;
	std::size_t jmpOffEnd = vaToOffset(jmpAddr);
	if (jmpOffEnd >= _size) return 0;

	for (std::size_t off = jmpOffEnd; off > jmpOff;)
	{
		--off;
		if (off + 2 >= _size) continue;
		uint8_t b = _data[off];

		// JA rel8 (77) or JAE rel8 (73) — check preceding instruction.
		if ((b == 0x77 || b == 0x73) && off >= 3)
		{
			// Look for CMP immediately before.
			// CMP r32, imm8: 83 F? <imm>  (3 bytes)
			if (_data[off - 3] == 0x83 && (_data[off - 2] & 0xF8) == 0xF8)
			{
				return static_cast<uint32_t>(_data[off - 1]) + 1; // bound = imm + 1
			}
			// CMP r64, imm8: 48 83 F? <imm>
			if (off >= 4 && _data[off - 4] == 0x48 && _data[off - 3] == 0x83 && (_data[off - 2] & 0xF8) == 0xF8)
			{
				return static_cast<uint32_t>(_data[off - 1]) + 1;
			}
		}
		// 0F 87/83 rel32 (JA/JAE near) — skip
	}
	return 0;
}

uint64_t CFGBuilder::detectJumpTableBase(uint64_t jmpAddr) const noexcept
{
	// Look backward for a LEA or MOV reg, [RIP+disp32] or MOV reg, imm64
	// that loads the table address.
	std::size_t jmpOff = vaToOffset(jmpAddr);
	if (jmpOff < 16) return 0;

	// `off > jmpOff - 32` is unsigned arithmetic on a std::size_t. The guard
	// above only promises jmpOff >= 16, so for a jump at file offset 16..31 the
	// subtraction wrapped to a value near SIZE_MAX and the condition was false
	// on the first test: the whole backward scan was skipped, and no jump table
	// in the first 32 bytes of an image was ever resolved. Count the steps
	// instead of comparing wrapped addresses.
	const std::size_t back = jmpOff < 32 ? jmpOff : 32;
	for (std::size_t step = 1; step < back; ++step)
	{
		const std::size_t off = jmpOff - step;
		if (off >= _size) continue;
		// LEA rX, [RIP+disp32]: 48 8D ?? <disp32>
		if (off + 7 <= _size && _data[off] == 0x48 && _data[off + 1] == 0x8D)
		{
			uint8_t modrm = _data[off + 2];
			if ((modrm & 0xC7) == 0x05)
			{ // RIP-relative
				int32_t disp = static_cast<int32_t>(
					static_cast<uint32_t>(_data[off + 3]) | (static_cast<uint32_t>(_data[off + 4]) << 8)
					| (static_cast<uint32_t>(_data[off + 5]) << 16) | (static_cast<uint32_t>(_data[off + 6]) << 24));
				uint64_t tableVA = _imageBase + (off + 7) + disp;
				return tableVA;
			}
		}
		// MOV rX, imm64: 48 B? <imm64>
		if (off + 10 <= _size && _data[off] == 0x48 && (_data[off + 1] >= 0xB8 && _data[off + 1] <= 0xBF))
		{
			uint64_t imm = 0;
			for (int i = 0; i < 8; ++i)
				imm |= static_cast<uint64_t>(_data[off + 2 + i]) << (i * 8);
			if (imm >= _imageBase && imm < _imageBase + _size) return imm;
		}
	}
	return 0;
}

void CFGBuilder::resolveJumpTables()
{
	// Both loops below mutate the graph while walking it, so each one scans
	// first and applies afterwards.  See the note on ResolvedTable.
	//
	// addEdge() appends to the very succs vector being iterated, which
	// reallocates it and dangles the loop's reference; ensureBlock() inserts
	// into _graph.nodes, which rehashes and invalidates the outer iterator.
	// Recording (block address, edge index) survives both: appends never move
	// an existing element, and unordered_map keeps references stable.
	std::vector<ResolvedTable> pending;

	// Process explicitly registered jump tables.
	for (const auto& jt: _jumpTables)
	{
		auto targets = resolveJumpTable(jt);
		if (targets.empty()) continue;

		// Find the block that owns the jump instruction.
		// The indirect JMP block will have an UnresolvedIndirect edge.
		for (const auto& [addr, blk]: _graph.nodes)
		{
			bool matched = false;
			for (std::size_t i = 0; i < blk.succs.size(); ++i)
			{
				if (blk.succs[i].type != EdgeType::UnresolvedIndirect) continue;
				// Match by proximity: the indirect JMP should be in a block
				// whose start is ≤ instrAddr < end.
				if (jt.instrAddr >= addr && (blk.endAddr == 0 || jt.instrAddr < blk.endAddr))
				{
					pending.push_back({addr, i, blk.functionAddr, targets});
					matched = true;
					break;
				}
			}
			if (matched)
			{
				// One registered table resolves at most one block, as before.
				break;
			}
		}
	}

	applyResolvedTables(pending);
	pending.clear();

	// Auto-detect remaining unresolved indirect JMPs (VSA-lite).
	for (const auto& [addr, blk]: _graph.nodes)
	{
		for (std::size_t i = 0; i < blk.succs.size(); ++i)
		{
			if (blk.succs[i].type != EdgeType::UnresolvedIndirect) continue;

			// Try to infer a jump table.
			uint32_t bound = detectJumpTableBound(blk.endAddr > 0 ? blk.endAddr - 1 : addr);
			if (bound == 0 || bound > 512) continue; // sanity limit

			uint64_t tableBase = detectJumpTableBase(blk.endAddr > 0 ? blk.endAddr - 1 : addr);
			if (tableBase == 0) continue;

			JumpTableInfo autoJT;
			autoJT.instrAddr = blk.endAddr > 0 ? blk.endAddr - 1 : addr;
			autoJT.tableBase = tableBase;
			autoJT.numEntries = bound;
			autoJT.stride = _is64Bit ? 8u : 4u;
			autoJT.fmt = JumpTableFmt::GCC;

			auto targets = resolveJumpTable(autoJT);
			if (targets.empty()) continue;

			pending.push_back({addr, i, blk.functionAddr, std::move(targets)});
		}
	}

	applyResolvedTables(pending);
}

void CFGBuilder::applyResolvedTables(const std::vector<ResolvedTable>& pending)
{
	for (const auto& r: pending)
	{
		auto it = _graph.nodes.find(r.block);
		if (it == _graph.nodes.end()) continue;
		if (r.edgeIndex >= it->second.succs.size()) continue;

		// Rewrite the placeholder in place, then append the remaining cases.
		// Appending cannot move the placeholder, so the recorded index stays
		// correct even though addEdge() may reallocate the vector.
		CFGEdge& edge = it->second.succs[r.edgeIndex];
		edge.type = EdgeType::SwitchEdge;
		edge.to = r.targets[0];
		edge.switchIndex = 0;

		for (std::size_t i = 1; i < r.targets.size(); ++i)
		{
			addEdge(r.block, r.targets[i], EdgeType::SwitchEdge, static_cast<uint32_t>(i));
			ensureBlock(r.targets[i], r.functionAddr);
		}
		ensureBlock(r.targets[0], r.functionAddr);
	}
}

// ─── Phase 2c: virtual call resolution ───────────────────────────────────────

void CFGBuilder::resolveVirtualCalls()
{
	if (_vtables.empty()) return;

	// Every non-null slot across every known vtable becomes an outgoing edge
	// of each unresolved indirect branch, so compute the slot list once.
	std::vector<uint64_t> slots;
	for (const auto& vt: _vtables)
	{
		for (const auto& slot: vt.slots)
		{
			if (slot != 0) slots.push_back(slot);
		}
	}
	if (slots.empty()) return;

	// Scan first, mutate second: addEdge() reallocates the succs vector this
	// loop iterates (a heap-use-after-free on the `edge` reference), and
	// ensureBlock() rehashes _graph.nodes under the outer iterator.
	std::vector<std::pair<uint64_t, std::size_t>> placeholders;
	for (const auto& [addr, blk]: _graph.nodes)
	{
		for (std::size_t i = 0; i < blk.succs.size(); ++i)
		{
			if (blk.succs[i].type == EdgeType::UnresolvedIndirect)
			{
				placeholders.emplace_back(addr, i);
			}
		}
	}

	const uint64_t firstSlot = _vtables[0].slots.empty() ? 0 : _vtables[0].slots[0];

	for (const auto& [addr, edgeIndex]: placeholders)
	{
		for (const uint64_t slot: slots)
		{
			addEdge(addr, slot, EdgeType::VirtualCallEdge);
			ensureBlock(slot, slot);
		}

		// Retire the placeholder.  Appends never move an existing element, so
		// the index recorded during the scan still addresses the same edge.
		auto it = _graph.nodes.find(addr);
		if (it == _graph.nodes.end()) continue;
		if (edgeIndex >= it->second.succs.size()) continue;
		it->second.succs[edgeIndex].type = EdgeType::VirtualCallEdge;
		it->second.succs[edgeIndex].to = firstSlot;
	}
}

// ─── Phase 2d: unresolved diagnostics ────────────────────────────────────────

void CFGBuilder::emitUnresolvedDiagnostics()
{
	for (const auto& [addr, blk]: _graph.nodes)
	{
		for (const auto& edge: blk.succs)
		{
			if (edge.type == EdgeType::UnresolvedIndirect)
			{
				CFGGraph::Diagnostic diag;
				diag.addr = addr;
				diag.reason = "Unresolved indirect branch at block 0x" + [addr] {
					char buf[32];
					std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)addr);
					return std::string(buf);
				}();
				_graph.diagnostics.push_back(diag);
			}
		}
	}
}

void CFGBuilder::runPhase2()
{
	resolveExceptionEdges();
	resolveJumpTables();
	resolveVirtualCalls();
	emitUnresolvedDiagnostics();
}

// ─── Phase 3: back-edge detection ────────────────────────────────────────────

void CFGBuilder::dfsVisit(uint64_t blockAddr, std::unordered_map<uint64_t, int>& colour)
{
	// An explicit stack, not the call stack. This walk recursed once per basic
	// block along a path, and a function that is one long chain of conditional
	// jumps -- which a 400 KB .text can easily be -- makes that path as long as
	// the block count: measured on this machine's 8 MB stack, a 60,000-block
	// chain was fine and a 200,000-block chain died with SIGSEGV. The colouring
	// is the same; only the bookkeeping moved.
	struct Frame
	{
		uint64_t addr;
		std::size_t next; ///< index of the next successor edge to look at
	};
	std::vector<Frame> stack;

	colour[blockAddr] = 1; // grey (in stack)
	stack.push_back({blockAddr, 0});

	while (!stack.empty())
	{
		Frame& top = stack.back();
		auto it = _graph.nodes.find(top.addr);
		if (it == _graph.nodes.end())
		{
			colour[top.addr] = 2;
			stack.pop_back();
			continue;
		}

		auto& succs = it->second.succs;
		bool descended = false;
		while (top.next < succs.size())
		{
			auto& edge = succs[top.next++];
			if (edge.to == 0) continue;
			if (edge.isCallEdge()) continue; // don't follow inter-procedural edges

			auto cit = colour.find(edge.to);
			if (cit == colour.end())
			{
				// Not visited.
				const uint64_t child = edge.to;
				colour[child] = 1;
				stack.push_back({child, 0});
				descended = true;
				break;
			}
			if (cit->second == 1)
			{
				// Grey = back edge → loop latch.
				edge.type = EdgeType::LoopBackEdge;
			}
			// Black = already done, forward/cross edge.
		}
		if (descended) continue;  // `top` is dangling after push_back

		colour[stack.back().addr] = 2; // black (done)
		stack.pop_back();
	}
}

void CFGBuilder::classifyBackEdges()
{
	// Run DFS from every function entry.
	for (const auto& fi: _functions)
	{
		if (_graph.nodes.find(fi.start) == _graph.nodes.end()) continue;
		std::unordered_map<uint64_t, int> colour;
		dfsVisit(fi.start, colour);
	}
}

void CFGBuilder::runPhase3()
{
	classifyBackEdges();
}

// ─── build() ─────────────────────────────────────────────────────────────────

void CFGBuilder::build()
{
	runPhase1();
	runPhase2();
	runPhase3();
}

} // namespace cfg
} // namespace retdec
