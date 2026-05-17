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
    for (const auto& [_, blk] : nodes)
        for (const auto& e : blk.succs)
            if (e.type == t) ++n;
    return n;
}

std::size_t CFGGraph::totalEdges() const noexcept
{
    std::size_t n = 0;
    for (const auto& [_, blk] : nodes) n += blk.succs.size();
    return n;
}

// ─── CFGBuilder constructor ───────────────────────────────────────────────────

CFGBuilder::CFGBuilder(uint64_t imageBase, const uint8_t* data,
                       std::size_t size, bool is64Bit)
    : _imageBase(imageBase), _data(data), _size(size), _is64Bit(is64Bit)
{}

// ─── Input registration ───────────────────────────────────────────────────────

void CFGBuilder::addFunction(uint64_t start, uint64_t end,
                              const std::vector<InstrSummary>& instrs)
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
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(_data[off+i]) << (i*8);
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
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(_data[off+i]) << (i*8);
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
    bb.startAddr    = addr;
    bb.endAddr      = addr; // filled in during Phase 1
    bb.functionAddr = funcStart;
    _graph.nodes[addr] = std::move(bb);
    return _graph.nodes[addr];
}

void CFGBuilder::addEdge(uint64_t from, uint64_t to, EdgeType type,
                          uint32_t switchIdx)
{
    CFGEdge e;
    e.from        = from;
    e.to          = to;
    e.type        = type;
    e.switchIndex = switchIdx;

    auto fit = _graph.nodes.find(from);
    if (fit != _graph.nodes.end()) {
        fit->second.succs.push_back(e);
    }
    if (to != 0) {
        auto tit = _graph.nodes.find(to);
        if (tit != _graph.nodes.end()) {
            tit->second.preds.push_back(from);
        }
    }
}

void CFGBuilder::splitBlockAt(uint64_t splitAddr)
{
    // Find the block containing splitAddr (but not starting at it).
    for (auto& [start, blk] : _graph.nodes) {
        if (start == splitAddr) return; // already a block boundary
        if (start < splitAddr && (blk.endAddr == 0 || splitAddr < blk.endAddr)) {
            // Split this block.
            BasicBlock newBlk;
            newBlk.startAddr    = splitAddr;
            newBlk.endAddr      = blk.endAddr;
            newBlk.functionAddr = blk.functionAddr;
            newBlk.succs        = std::move(blk.succs);
            // Update preds of original successors.
            for (const auto& e : newBlk.succs) {
                if (e.to != 0) {
                    auto sit = _graph.nodes.find(e.to);
                    if (sit != _graph.nodes.end()) {
                        auto& preds = sit->second.preds;
                        // Replace old start with newBlk.startAddr.
                        for (auto& p : preds) {
                            if (p == start) { p = splitAddr; break; }
                        }
                    }
                }
            }
            blk.endAddr = splitAddr;
            blk.succs.clear();

            _graph.nodes[splitAddr] = std::move(newBlk);

            // Add fallthrough edge from original block to new block.
            addEdge(start, splitAddr, EdgeType::FallThrough);
            return;
        }
    }
}

// ─── Phase 1 ──────────────────────────────────────────────────────────────────

void CFGBuilder::buildBlocksForFunction(const FunctionInfo& fi)
{
    if (fi.instrs.empty()) {
        ensureBlock(fi.start, fi.start).endAddr = fi.end;
        return;
    }

    uint64_t currentBlockStart = fi.start;
    ensureBlock(currentBlockStart, fi.start);

    for (std::size_t i = 0; i < fi.instrs.size(); ++i) {
        const InstrSummary& ins = fi.instrs[i];
        uint64_t nextAddr = ins.addr + ins.len;

        // Update end address of current block.
        {
            auto it = _graph.nodes.find(currentBlockStart);
            if (it != _graph.nodes.end()) {
                it->second.endAddr = nextAddr;
            }
        }

        switch (ins.kind) {
        case InstrKind::Normal:
        case InstrKind::IndirectCall:
            // IndirectCall: emit an unresolved indirect edge but continue
            // the basic block (fallthrough to next instruction).
            if (ins.kind == InstrKind::IndirectCall) {
                addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
            }
            break;

        case InstrKind::DirectCall:
            // Emit DirectCall edge; block continues with fallthrough.
            if (ins.target != 0) {
                // Ensure target exists before addEdge so preds are recorded.
                ensureBlock(ins.target, ins.target);
                addEdge(currentBlockStart, ins.target, EdgeType::DirectCall);
            }
            // FallThrough to next instruction stays in same block — no split.
            break;

        case InstrKind::DirectJmp:
            if (ins.target != 0) {
                // Ensure target exists before addEdge so preds are recorded.
                ensureBlock(ins.target, fi.start);
                addEdge(currentBlockStart, ins.target, EdgeType::TrueBranch);
                // If target is mid-block, split.
                splitBlockAt(ins.target);
            } else {
                addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
            }
            // Start new block at next instruction if more instrs follow.
            // Use the actual next instruction address (not nextAddr) to handle
            // non-sequential instruction layouts (e.g. gaps between blocks).
            if (i + 1 < fi.instrs.size()) {
                currentBlockStart = fi.instrs[i + 1].addr;
                ensureBlock(currentBlockStart, fi.start);
            }
            break;

        case InstrKind::ConditionalJmp:
            if (ins.target != 0) {
                // Ensure target exists before addEdge so preds are recorded.
                ensureBlock(ins.target, fi.start);
                addEdge(currentBlockStart, ins.target, EdgeType::TrueBranch);
                splitBlockAt(ins.target);
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
                if (hit != _exHandlers.end()) {
                    addEdge(currentBlockStart, hit->second, EdgeType::ExceptionEdge);
                } else {
                    addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
                }
            }
            if (i + 1 < fi.instrs.size()) {
                currentBlockStart = fi.instrs[i + 1].addr;
                ensureBlock(currentBlockStart, fi.start);
            }
            break;

        case InstrKind::TailCall:
            if (ins.target != 0) {
                ensureBlock(ins.target, ins.target);
                addEdge(currentBlockStart, ins.target, EdgeType::TailCall);
            } else {
                addEdge(currentBlockStart, 0, EdgeType::UnresolvedIndirect);
            }
            if (i + 1 < fi.instrs.size()) {
                currentBlockStart = fi.instrs[i + 1].addr;
                ensureBlock(currentBlockStart, fi.start);
            }
            break;

        case InstrKind::Ret:
            // No outgoing CFG edge (function exit).
            // Use the actual next instruction's address to avoid creating
            // phantom blocks when there is a gap between this Ret and the
            // next instruction (e.g. separate basic blocks in the function).
            if (i + 1 < fi.instrs.size()) {
                currentBlockStart = fi.instrs[i + 1].addr;
                ensureBlock(currentBlockStart, fi.start);
            }
            break;
        }
    }
}

void CFGBuilder::runPhase1()
{
    for (const auto& fi : _functions) {
        buildBlocksForFunction(fi);
    }
}

// ─── Phase 2a: exception edges ────────────────────────────────────────────────

void CFGBuilder::resolveExceptionEdges()
{
    for (auto& [addr, blk] : _graph.nodes) {
        for (auto& edge : blk.succs) {
            if (edge.type != EdgeType::UnresolvedIndirect) continue;
            // Check if this block's last indirect JMP has a registered handler.
            auto hit = _exHandlers.find(addr);
            if (hit != _exHandlers.end()) {
                edge.type = EdgeType::ExceptionEdge;
                edge.to   = hit->second;
                // Update pred list.
                auto tit = _graph.nodes.find(hit->second);
                if (tit != _graph.nodes.end()) {
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

    for (uint32_t i = 0; i < jt.numEntries; ++i) {
        uint64_t entryVA = base + static_cast<uint64_t>(i) * stride;
        if (!inBounds(entryVA, stride)) break;

        uint64_t target = 0;
        switch (jt.fmt) {
        case JumpTableFmt::GCC:
        case JumpTableFmt::Clang:
            target = (stride == 8) ? readU64(entryVA) : readU32(entryVA);
            break;
        case JumpTableFmt::MSVC:
            // Signed 32-bit offset from table base.
            target = static_cast<uint64_t>(
                static_cast<int64_t>(base) + static_cast<int64_t>(readI32(entryVA)));
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
    if (jmpOff > 64) jmpOff -= 32; else jmpOff = 0;
    std::size_t jmpOffEnd = vaToOffset(jmpAddr);
    if (jmpOffEnd >= _size) return 0;

    for (std::size_t off = jmpOffEnd; off > jmpOff; ) {
        --off;
        if (off + 2 >= _size) continue;
        uint8_t b = _data[off];

        // JA rel8 (77) or JAE rel8 (73) — check preceding instruction.
        if ((b == 0x77 || b == 0x73) && off >= 3) {
            // Look for CMP immediately before.
            // CMP r32, imm8: 83 F? <imm>  (3 bytes)
            if (_data[off-3] == 0x83 && (_data[off-2] & 0xF8) == 0xF8) {
                return static_cast<uint32_t>(_data[off-1]) + 1; // bound = imm + 1
            }
            // CMP r64, imm8: 48 83 F? <imm>
            if (off >= 4 && _data[off-4] == 0x48 &&
                _data[off-3] == 0x83 && (_data[off-2] & 0xF8) == 0xF8) {
                return static_cast<uint32_t>(_data[off-1]) + 1;
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

    for (std::size_t off = jmpOff - 1; off > jmpOff - 32 && off < _size; --off) {
        // LEA rX, [RIP+disp32]: 48 8D ?? <disp32>
        if (off + 7 <= _size &&
            _data[off] == 0x48 && _data[off+1] == 0x8D) {
            uint8_t modrm = _data[off+2];
            if ((modrm & 0xC7) == 0x05) { // RIP-relative
                int32_t disp = static_cast<int32_t>(
                    static_cast<uint32_t>(_data[off+3]) |
                    (static_cast<uint32_t>(_data[off+4]) << 8) |
                    (static_cast<uint32_t>(_data[off+5]) << 16) |
                    (static_cast<uint32_t>(_data[off+6]) << 24));
                uint64_t tableVA = _imageBase + (off + 7) + disp;
                return tableVA;
            }
        }
        // MOV rX, imm64: 48 B? <imm64>
        if (off + 10 <= _size && _data[off] == 0x48 &&
            (_data[off+1] >= 0xB8 && _data[off+1] <= 0xBF)) {
            uint64_t imm = 0;
            for (int i = 0; i < 8; ++i)
                imm |= static_cast<uint64_t>(_data[off+2+i]) << (i*8);
            if (imm >= _imageBase && imm < _imageBase + _size)
                return imm;
        }
    }
    return 0;
}

void CFGBuilder::resolveJumpTables()
{
    // Process explicitly registered jump tables.
    for (const auto& jt : _jumpTables) {
        auto targets = resolveJumpTable(jt);
        if (targets.empty()) continue;

        // Find the block that owns the jump instruction.
        // The indirect JMP block will have an UnresolvedIndirect edge.
        for (auto& [addr, blk] : _graph.nodes) {
            for (auto& edge : blk.succs) {
                if (edge.type != EdgeType::UnresolvedIndirect) continue;
                // Match by proximity: the indirect JMP should be in a block
                // whose start is ≤ instrAddr < end.
                if (jt.instrAddr >= addr &&
                    (blk.endAddr == 0 || jt.instrAddr < blk.endAddr)) {
                    // Replace with SwitchEdges.
                    edge.type = EdgeType::SwitchEdge; // reuse first slot
                    edge.to   = targets[0];
                    edge.switchIndex = 0;
                    for (std::size_t i = 1; i < targets.size(); ++i) {
                        addEdge(addr, targets[i], EdgeType::SwitchEdge,
                                static_cast<uint32_t>(i));
                        // Ensure target block exists.
                        ensureBlock(targets[i], blk.functionAddr);
                    }
                    ensureBlock(targets[0], blk.functionAddr);
                    break;
                }
            }
        }
    }

    // Auto-detect remaining unresolved indirect JMPs (VSA-lite).
    for (auto& [addr, blk] : _graph.nodes) {
        for (auto& edge : blk.succs) {
            if (edge.type != EdgeType::UnresolvedIndirect) continue;

            // Try to infer a jump table.
            uint32_t bound = detectJumpTableBound(blk.endAddr > 0 ? blk.endAddr - 1 : addr);
            if (bound == 0 || bound > 512) continue; // sanity limit

            uint64_t tableBase = detectJumpTableBase(blk.endAddr > 0 ? blk.endAddr - 1 : addr);
            if (tableBase == 0) continue;

            JumpTableInfo autoJT;
            autoJT.instrAddr  = blk.endAddr > 0 ? blk.endAddr - 1 : addr;
            autoJT.tableBase  = tableBase;
            autoJT.numEntries = bound;
            autoJT.stride     = _is64Bit ? 8u : 4u;
            autoJT.fmt        = JumpTableFmt::GCC;

            auto targets = resolveJumpTable(autoJT);
            if (targets.empty()) continue;

            edge.type        = EdgeType::SwitchEdge;
            edge.to          = targets[0];
            edge.switchIndex = 0;
            ensureBlock(targets[0], blk.functionAddr);

            for (std::size_t i = 1; i < targets.size(); ++i) {
                addEdge(addr, targets[i], EdgeType::SwitchEdge,
                        static_cast<uint32_t>(i));
                ensureBlock(targets[i], blk.functionAddr);
            }
        }
    }
}

// ─── Phase 2c: virtual call resolution ───────────────────────────────────────

void CFGBuilder::resolveVirtualCalls()
{
    if (_vtables.empty()) return;

    for (auto& [addr, blk] : _graph.nodes) {
        for (auto& edge : blk.succs) {
            if (edge.type != EdgeType::UnresolvedIndirect) continue;
            // Emit one VirtualCallEdge per vtable slot across all known vtables.
            bool anyVtable = false;
            for (const auto& vt : _vtables) {
                for (const auto& slot : vt.slots) {
                    if (slot == 0) continue;
                    addEdge(addr, slot, EdgeType::VirtualCallEdge);
                    ensureBlock(slot, slot);
                    anyVtable = true;
                }
            }
            if (anyVtable) {
                // Remove the original unresolved placeholder (mark as resolved).
                edge.type = EdgeType::VirtualCallEdge;
                edge.to   = _vtables[0].slots.empty() ? 0 : _vtables[0].slots[0];
            }
        }
    }
}

// ─── Phase 2d: unresolved diagnostics ────────────────────────────────────────

void CFGBuilder::emitUnresolvedDiagnostics()
{
    for (const auto& [addr, blk] : _graph.nodes) {
        for (const auto& edge : blk.succs) {
            if (edge.type == EdgeType::UnresolvedIndirect) {
                CFGGraph::Diagnostic diag;
                diag.addr   = addr;
                diag.reason = "Unresolved indirect branch at block 0x" +
                              [addr]{ char buf[32];
                                      std::snprintf(buf,sizeof(buf),"%llx",
                                                    (unsigned long long)addr);
                                      return std::string(buf); }();
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

void CFGBuilder::dfsVisit(uint64_t blockAddr,
                           std::unordered_map<uint64_t, int>& colour)
{
    colour[blockAddr] = 1; // grey (in stack)

    auto it = _graph.nodes.find(blockAddr);
    if (it == _graph.nodes.end()) { colour[blockAddr] = 2; return; }

    for (auto& edge : it->second.succs) {
        if (edge.to == 0) continue;
        if (edge.isCallEdge()) continue; // don't follow inter-procedural edges

        auto cit = colour.find(edge.to);
        if (cit == colour.end()) {
            // Not visited.
            dfsVisit(edge.to, colour);
        } else if (cit->second == 1) {
            // Grey = back edge → loop latch.
            edge.type = EdgeType::LoopBackEdge;
        }
        // Black = already done, forward/cross edge.
    }
    colour[blockAddr] = 2; // black (done)
}

void CFGBuilder::classifyBackEdges()
{
    // Run DFS from every function entry.
    for (const auto& fi : _functions) {
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
