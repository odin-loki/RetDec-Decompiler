/**
 * @file src/bc_module/bc_cfg.cpp
 * @brief BcCFG — control-flow graph implementation.
 */

#include "retdec/bc_module/bc_cfg.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace bc_module {

// ─── BcBasicBlock ─────────────────────────────────────────────────────────────

bool BcBasicBlock::hasTerminator() const noexcept {
    if (instrs.empty()) return false;
    switch (instrs.back().opcode) {
    case BcOpcode::Goto:
    case BcOpcode::IfTrue:   case BcOpcode::IfFalse:
    case BcOpcode::IfNull:   case BcOpcode::IfNonNull:
    case BcOpcode::IfEq:     case BcOpcode::IfNe:
    case BcOpcode::IfLt:     case BcOpcode::IfGe:
    case BcOpcode::IfGt:     case BcOpcode::IfLe:
    case BcOpcode::TableSwitch: case BcOpcode::LookupSwitch:
    case BcOpcode::Return:   case BcOpcode::ReturnValue:
    case BcOpcode::Throw:
    case BcOpcode::LuaReturn: case BcOpcode::LuaTailCall:
        return true;
    default:
        return false;
    }
}

BcInstruction* BcBasicBlock::terminator() {
    if (instrs.empty() || !hasTerminator()) return nullptr;
    return &instrs.back();
}

const BcInstruction* BcBasicBlock::terminator() const {
    if (instrs.empty() || !hasTerminator()) return nullptr;
    return &instrs.back();
}

// ─── BcCFG ────────────────────────────────────────────────────────────────────

BcBasicBlock& BcCFG::addBlock() {
    BcBasicBlock blk;
    blk.id = static_cast<uint32_t>(blocks_.size());
    blocks_.push_back(std::move(blk));
    return blocks_.back();
}

BcBasicBlock& BcCFG::block(uint32_t id) {
    return blocks_.at(id);
}

const BcBasicBlock& BcCFG::block(uint32_t id) const {
    return blocks_.at(id);
}

void BcCFG::addEdge(uint32_t from, uint32_t to) {
    auto& f = blocks_.at(from);
    auto& t = blocks_.at(to);
    if (std::find(f.succs.begin(), f.succs.end(), to) == f.succs.end())
        f.succs.push_back(to);
    if (std::find(t.preds.begin(), t.preds.end(), from) == t.preds.end())
        t.preds.push_back(from);
}

void BcCFG::removeEdge(uint32_t from, uint32_t to) {
    auto& f = blocks_.at(from);
    auto& t = blocks_.at(to);
    f.succs.erase(std::remove(f.succs.begin(), f.succs.end(), to), f.succs.end());
    t.preds.erase(std::remove(t.preds.begin(), t.preds.end(), from), t.preds.end());
}

bool BcCFG::hasEdge(uint32_t from, uint32_t to) const {
    if (from >= blocks_.size()) return false;
    const auto& s = blocks_[from].succs;
    return std::find(s.begin(), s.end(), to) != s.end();
}

void BcCFG::addExceptionHandler(BcExceptionHandler eh) {
    handlers_.push_back(std::move(eh));
}

uint32_t BcCFG::blockOfOffset(uint32_t offset) const {
    // Linear scan (offset map built by buildOffsetMap).
    for (const auto& [off, id] : offsetMap_)
        if (off <= offset) {
            // Check the block's instruction range.
            const auto& blk = blocks_[id];
            if (!blk.instrs.empty() && blk.instrs.front().offset <= offset &&
                (blk.instrs.back().offset >= offset))
                return id;
        }
    return UINT32_MAX;
}

void BcCFG::buildOffsetMap() {
    offsetMap_.clear();
    for (const auto& blk : blocks_) {
        if (!blk.instrs.empty())
            offsetMap_.push_back({blk.instrs.front().offset, blk.id});
    }
    std::sort(offsetMap_.begin(), offsetMap_.end());
}

bool BcCFG::verify(std::string& error) const {
    // Every block referenced by an edge must exist.
    for (const auto& blk : blocks_) {
        for (uint32_t s : blk.succs) {
            if (s >= blocks_.size()) {
                error = "Block " + std::to_string(blk.id)
                      + " has invalid successor " + std::to_string(s);
                return false;
            }
        }
        for (uint32_t p : blk.preds) {
            if (p >= blocks_.size()) {
                error = "Block " + std::to_string(blk.id)
                      + " has invalid predecessor " + std::to_string(p);
                return false;
            }
        }
        // Predecessor symmetry: if A is a successor of B, B must be a pred of A.
        for (uint32_t s : blk.succs) {
            const auto& sblk = blocks_[s];
            if (std::find(sblk.preds.begin(), sblk.preds.end(), blk.id) == sblk.preds.end()) {
                error = "Asymmetric edge " + std::to_string(blk.id)
                      + " → " + std::to_string(s) + " (missing reverse)";
                return false;
            }
        }
    }
    // Exception handler blocks must exist.
    for (const auto& eh : handlers_) {
        if (eh.handlerBlock >= blocks_.size()) {
            error = "Exception handler references nonexistent block "
                  + std::to_string(eh.handlerBlock);
            return false;
        }
    }
    return true;
}

} // namespace bc_module
} // namespace retdec
