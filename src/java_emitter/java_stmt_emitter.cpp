/**
 * @file src/java_emitter/java_stmt_emitter.cpp
 * @brief Java statement emitter — walks BcCFG and emits Java statements.
 */

#include "retdec/java_emitter/java_stmt_emitter.h"

#include <algorithm>
#include <cassert>

namespace retdec {
namespace java_emitter {

using namespace bc_module;
using namespace jvm_reconstruct;

// ─── CodeWriter ──────────────────────────────────────────────────────────────

CodeWriter::CodeWriter(int indentWidth)
    : level_(0), indentWidth_(indentWidth) {
    updateIndentStr();
}

void CodeWriter::updateIndentStr() {
    indentStr_ = std::string(static_cast<size_t>(level_ * indentWidth_), ' ');
}

void CodeWriter::indent() { ++level_; updateIndentStr(); }
void CodeWriter::dedent() { if (level_ > 0) { --level_; updateIndentStr(); } }

void CodeWriter::writeLine(const std::string& line) {
    if (line.empty()) {
        buf_ << "\n";
    } else {
        buf_ << indentStr_ << line << "\n";
    }
}

void CodeWriter::writeLine() {
    buf_ << "\n";
}

void CodeWriter::write(const std::string& s) {
    buf_ << s;
}

std::string CodeWriter::str() const {
    return buf_.str();
}

// ─── JavaStmtEmitter ─────────────────────────────────────────────────────────

JavaStmtEmitter::JavaStmtEmitter(const BcMethod& method,
                                   const ReconstructResult& recon,
                                   const JavaTypePrinter& tyPrinter,
                                   const StmtEmitOptions& opts)
    : method_(method), recon_(recon), tyPrinter_(tyPrinter), opts_(opts),
      exprCtx_(method, recon, tyPrinter),
      exprEmit_(exprCtx_) {
    buildPatternMaps();
}

void JavaStmtEmitter::buildPatternMaps() {
    for (size_t i = 0; i < recon_.patterns.forEachLoops.size(); ++i)
        forEachByHeader_[recon_.patterns.forEachLoops[i].loopHeaderBlock] = i;
    for (size_t i = 0; i < recon_.patterns.stringConcats.size(); ++i)
        stringConcatByBlock_[recon_.patterns.stringConcats[i].blockId] = i;
    for (size_t i = 0; i < recon_.patterns.lambdas.size(); ++i)
        lambdaByBlock_[recon_.patterns.lambdas[i].blockId] = i;
}

// ─── CFG helpers ─────────────────────────────────────────────────────────────

bool JavaStmtEmitter::isLoopHeader(uint32_t blockId) const {
    if (blockId >= cfg().blockCount()) return false;
    return cfg().block(blockId).isLoopHeader;
}

uint32_t JavaStmtEmitter::backEdgeSource(uint32_t blockId) const {
    if (blockId >= cfg().blockCount()) return UINT32_MAX;
    const BcBasicBlock& hdr = cfg().block(blockId);
    for (uint32_t pred : hdr.preds) {
        if (pred >= blockId) // Back edge: pred comes after header in block order.
            return pred;
    }
    return UINT32_MAX;
}

uint32_t JavaStmtEmitter::loopExit(uint32_t blockId) const {
    if (blockId >= cfg().blockCount()) return UINT32_MAX;
    const BcBasicBlock& hdr = cfg().block(blockId);
    // The exit is the successor not in the loop (the one with higher id than the back-edge src).
    for (uint32_t succ : hdr.succs) {
        if (succ > blockId)
            return succ;
    }
    return UINT32_MAX;
}

uint32_t JavaStmtEmitter::findJoin(uint32_t blockId) const {
    // Simplified: the join point of an if-else is the common successor.
    if (blockId >= cfg().blockCount()) return UINT32_MAX;
    const BcBasicBlock& blk = cfg().block(blockId);
    if (blk.succs.size() < 2) return UINT32_MAX;

    // Find the first block that both successors eventually reach.
    // Simple heuristic: it's max(succ0, succ1) + 1 if they don't share a direct succ.
    uint32_t a = blk.succs[0];
    uint32_t b = blk.succs[1];
    if (a > b) std::swap(a, b);

    // Check if b is the join (a falls through to b).
    if (a < cfg().blockCount()) {
        const BcBasicBlock& ablk = cfg().block(a);
        for (uint32_t asucc : ablk.succs)
            if (asucc == b) return b;
    }
    return b; // Best guess.
}

// ─── Block emission ───────────────────────────────────────────────────────────

void JavaStmtEmitter::flushStack(std::vector<ExprNode>& stack) {
    // Emit any side-effecting expressions left on the stack as statements.
    while (!stack.empty()) {
        auto node = stack.back();
        stack.pop_back();
        if (node.sideEffects)
            out_.writeLine(node.text + ";");
    }
}

std::string JavaStmtEmitter::buildCondition(const BcInstruction& branchInsn,
                                              std::vector<ExprNode>& stack) {
    switch (branchInsn.opcode) {
        case BcOpcode::IfTrue: {
            if (!stack.empty()) {
                auto e = stack.back(); stack.pop_back();
                return e.text;
            }
            return "/* cond */";
        }
        case BcOpcode::IfFalse: {
            if (!stack.empty()) {
                auto e = stack.back(); stack.pop_back();
                return "!" + e.text;
            }
            return "/* cond */";
        }
        case BcOpcode::IfEq: case BcOpcode::CmpEq: {
            if (stack.size() >= 2) {
                auto rhs = stack.back(); stack.pop_back();
                auto lhs = stack.back(); stack.pop_back();
                return lhs.text + " == " + rhs.text;
            }
            return "/* == */";
        }
        case BcOpcode::IfNe: case BcOpcode::CmpNe: {
            if (stack.size() >= 2) {
                auto rhs = stack.back(); stack.pop_back();
                auto lhs = stack.back(); stack.pop_back();
                return lhs.text + " != " + rhs.text;
            }
            return "/* != */";
        }
        case BcOpcode::IfLt: case BcOpcode::CmpLt: {
            if (stack.size() >= 2) {
                auto rhs = stack.back(); stack.pop_back();
                auto lhs = stack.back(); stack.pop_back();
                return lhs.text + " < " + rhs.text;
            }
            return "/* < */";
        }
        case BcOpcode::IfGe: case BcOpcode::CmpGe: {
            if (stack.size() >= 2) {
                auto rhs = stack.back(); stack.pop_back();
                auto lhs = stack.back(); stack.pop_back();
                return lhs.text + " >= " + rhs.text;
            }
            return "/* >= */";
        }
        case BcOpcode::IfGt: case BcOpcode::CmpGt: {
            if (stack.size() >= 2) {
                auto rhs = stack.back(); stack.pop_back();
                auto lhs = stack.back(); stack.pop_back();
                return lhs.text + " > " + rhs.text;
            }
            return "/* > */";
        }
        case BcOpcode::IfLe: case BcOpcode::CmpLe: {
            if (stack.size() >= 2) {
                auto rhs = stack.back(); stack.pop_back();
                auto lhs = stack.back(); stack.pop_back();
                return lhs.text + " <= " + rhs.text;
            }
            return "/* <= */";
        }
        default:
            return "/* cond */";
    }
}

bool JavaStmtEmitter::emitInstrAsStmt(const BcInstruction& insn,
                                       std::vector<ExprNode>& exprStack) {
    std::string expr = exprEmit_.emitInsn(insn, exprStack);
    if (expr.empty()) return false; // Purely internal (dup, swap, etc.)

    // Check if this is a statement-producing instruction.
    switch (insn.opcode) {
        case BcOpcode::StoreLocal:
        case BcOpcode::ArrayStore:
        case BcOpcode::PutField:
        case BcOpcode::PutStatic:
        case BcOpcode::Return:
        case BcOpcode::ReturnValue:
        case BcOpcode::Throw:
        case BcOpcode::MonitorEnter:
        case BcOpcode::MonitorExit:
            out_.writeLine(expr + ";");
            return true;

        // Void-returning invocations.
        case BcOpcode::InvokeVirtual:
        case BcOpcode::InvokeInterface:
        case BcOpcode::InvokeSpecial:
        case BcOpcode::InvokeStatic:
        case BcOpcode::InvokeDynamic:
        case BcOpcode::Callvirt:
        case BcOpcode::Call: {
            bool returnsVoid = true;
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    returnsVoid = !m->descriptor.returnType ||
                                  m->descriptor.returnType->isVoid();
            }
            if (returnsVoid) {
                out_.writeLine(expr + ";");
                return true;
            }
            return false; // Value pushed to stack.
        }

        default:
            return false; // Expression result on stack.
    }
}

void JavaStmtEmitter::emitBlock(uint32_t blockId) {
    if (blockId >= cfg().blockCount()) return;
    const BcBasicBlock& blk = cfg().block(blockId);

    std::vector<ExprNode> exprStack;

    for (size_t i = 0; i < blk.instrs.size(); ++i) {
        const auto& insn = blk.instrs[i];

        // Skip branching instructions — handled structurally.
        if (insn.opcode == BcOpcode::Goto ||
            insn.opcode == BcOpcode::IfTrue || insn.opcode == BcOpcode::IfFalse ||
            insn.opcode == BcOpcode::IfEq   || insn.opcode == BcOpcode::IfNe   ||
            insn.opcode == BcOpcode::IfLt   || insn.opcode == BcOpcode::IfGe   ||
            insn.opcode == BcOpcode::IfGt   || insn.opcode == BcOpcode::IfLe   ||
            insn.opcode == BcOpcode::TableSwitch ||
            insn.opcode == BcOpcode::LookupSwitch)
            continue;

        // Declare local variable on first store.
        if (insn.opcode == BcOpcode::StoreLocal && !insn.operands.empty()) {
            if (auto* lop = std::get_if<BcLocalOperand>(&insn.operands[0])) {
                uint32_t idx = lop->index;
                auto nameIt = exprCtx_.localNames.find(idx);
                auto typeIt = exprCtx_.localTypes.find(idx);
                if (nameIt != exprCtx_.localNames.end() &&
                    typeIt != exprCtx_.localTypes.end()) {
                    std::string typeName = tyPrinter_.print(typeIt->second);
                    ExprNode val = exprStack.empty()
                        ? ExprNode{ExprKind::Literal, "/* ? */", 0, false}
                        : (exprStack.back(), exprStack.pop_back(),
                           exprStack.size() < 1000 /* always true */
                               ? ExprNode{ExprKind::Literal, "/* extracted */", 0, false}
                               : ExprNode{ExprKind::Literal, "/* ? */", 0, false});
                    // Simpler: just emit the store expression.
                    std::string expr = exprEmit_.emitInsn(insn, exprStack);
                    if (!expr.empty())
                        out_.writeLine(typeName + " " + expr + ";");
                    continue;
                }
            }
        }

        emitInstrAsStmt(insn, exprStack);
    }

    flushStack(exprStack);
}

// ─── Structural control-flow emission ────────────────────────────────────────

bool JavaStmtEmitter::tryEmitForEach(uint32_t blockId) {
    if (!opts_.emitEnhancedFor) return false;
    auto it = forEachByHeader_.find(blockId);
    if (it == forEachByHeader_.end()) return false;
    const ForEachPattern& pat = recon_.patterns.forEachLoops[it->second];

    std::string elemType = tyPrinter_.print(pat.elementType);
    auto nameIt = exprCtx_.localNames.find(pat.elementSlot);
    std::string elemName = (nameIt != exprCtx_.localNames.end())
                           ? nameIt->second : "item";

    auto collNameIt = exprCtx_.localNames.find(pat.collectionSlot);
    std::string collExpr = (collNameIt != exprCtx_.localNames.end())
                           ? collNameIt->second : "collection";

    out_.writeLine("for (" + elemType + " " + elemName + " : " + collExpr + ") {");
    out_.indent();
    visited_.insert(blockId);
    emitFrom(pat.bodyBlock, pat.exitBlock);
    out_.dedent();
    out_.writeLine("}");

    visited_.insert(blockId);
    if (pat.exitBlock != UINT32_MAX)
        emitFrom(pat.exitBlock);
    return true;
}

bool JavaStmtEmitter::tryEmitWhile(uint32_t blockId) {
    if (!isLoopHeader(blockId)) return false;
    if (forEachByHeader_.count(blockId)) return false; // Handled by for-each.

    uint32_t exitBlock = loopExit(blockId);
    uint32_t backSrc   = backEdgeSource(blockId);
    if (backSrc == UINT32_MAX) return false;

    // Build the condition from the header's conditional branch.
    const BcBasicBlock& hdr = cfg().block(blockId);
    std::string cond = "true";
    std::vector<ExprNode> tmpStack;

    for (const auto& insn : hdr.instrs) {
        if (insn.opcode == BcOpcode::IfFalse || insn.opcode == BcOpcode::IfTrue) {
            cond = buildCondition(insn, tmpStack);
            break;
        }
    }

    out_.writeLine("while (" + cond + ") {");
    out_.indent();
    visited_.insert(blockId);

    // Emit loop body: all blocks until back-edge.
    if (!hdr.succs.empty()) {
        uint32_t bodyStart = hdr.succs[0];
        if (bodyStart == exitBlock && hdr.succs.size() >= 2)
            bodyStart = hdr.succs[1];
        emitFrom(bodyStart, blockId);
    }

    out_.dedent();
    out_.writeLine("}");

    if (exitBlock != UINT32_MAX)
        emitFrom(exitBlock);
    return true;
}

bool JavaStmtEmitter::tryEmitIfElse(uint32_t blockId) {
    if (blockId >= cfg().blockCount()) return false;
    const BcBasicBlock& blk = cfg().block(blockId);
    if (blk.succs.size() < 2) return false;
    if (isLoopHeader(blockId)) return false;

    uint32_t thenBlock = blk.succs[0];
    uint32_t elseBlock = blk.succs[1];
    uint32_t joinBlock  = findJoin(blockId);

    // Build condition.
    std::vector<ExprNode> tmpStack;
    std::string cond = "/* cond */";
    const BcInstruction* branchInsn = nullptr;
    for (auto& insn : blk.instrs) {
        if (insn.opcode == BcOpcode::IfTrue || insn.opcode == BcOpcode::IfFalse ||
            insn.opcode == BcOpcode::IfEq   || insn.opcode == BcOpcode::IfNe   ||
            insn.opcode == BcOpcode::IfLt   || insn.opcode == BcOpcode::IfGe   ||
            insn.opcode == BcOpcode::IfGt   || insn.opcode == BcOpcode::IfLe) {
            branchInsn = &insn;
            break;
        }
    }
    if (branchInsn) {
        // First emit non-branch instructions to build stack.
        for (const auto& insn : blk.instrs) {
            if (&insn == branchInsn) break;
            exprEmit_.emitInsn(const_cast<BcInstruction&>(insn), tmpStack);
        }
        cond = buildCondition(*branchInsn, tmpStack);
    }

    out_.writeLine("if (" + cond + ") {");
    out_.indent();
    visited_.insert(blockId);
    emitFrom(thenBlock, joinBlock);
    out_.dedent();

    // Emit else if it has its own block.
    if (elseBlock != joinBlock && elseBlock < cfg().blockCount() &&
        !visited_.count(elseBlock)) {
        out_.writeLine("} else {");
        out_.indent();
        emitFrom(elseBlock, joinBlock);
        out_.dedent();
    }
    out_.writeLine("}");

    if (joinBlock != UINT32_MAX && joinBlock < cfg().blockCount())
        emitFrom(joinBlock);
    return true;
}

bool JavaStmtEmitter::tryEmitTryCatch(uint32_t blockId) {
    // Find exception handlers that start at this block.
    const auto& handlers = cfg().handlers();
    bool found = false;
    for (const auto& eh : handlers) {
        if (blockId < cfg().blockCount() &&
            cfg().block(blockId).id >= eh.startOffset) {
            found = true; break;
        }
    }
    if (!found) return false;

    out_.writeLine("try {");
    out_.indent();
    emitBlock(blockId);
    out_.dedent();

    for (const auto& eh : handlers) {
        if (eh.handlerBlock >= cfg().blockCount()) continue;
        std::string catchType = "Throwable";
        if (eh.catchType.has_value())
            catchType = tyPrinter_.print(*eh.catchType);

        // Find the exception variable name.
        std::string exVarName = "ex";
        for (const auto& lv : method_.locals) {
            if (lv.name.find("ex") != std::string::npos && !lv.isParam) {
                exVarName = lv.name; break;
            }
        }

        if (eh.isFinally) {
            out_.writeLine("} finally {");
        } else {
            out_.writeLine("} catch (" + catchType + " " + exVarName + ") {");
        }
        out_.indent();
        if (!visited_.count(eh.handlerBlock))
            emitFrom(eh.handlerBlock);
        out_.dedent();
    }
    out_.writeLine("}");
    return true;
}

bool JavaStmtEmitter::tryEmitSwitch(uint32_t /*blockId*/) {
    return false; // Placeholder — full switch requires CFG analysis.
}

bool JavaStmtEmitter::tryEmitDoWhile(uint32_t /*blockId*/) {
    return false; // Placeholder — do-while requires back-edge analysis.
}

bool JavaStmtEmitter::tryEmitFor(uint32_t /*blockId*/) {
    return false; // Placeholder — classical for loop recognition.
}

bool JavaStmtEmitter::tryEmitSynchronized(uint32_t /*blockId*/) {
    return false; // Placeholder — monitor enter/exit pair detection.
}

// ─── emitFrom ────────────────────────────────────────────────────────────────

void JavaStmtEmitter::emitFrom(uint32_t id, uint32_t stopBlock) {
    while (id < cfg().blockCount() && id != stopBlock && !visited_.count(id)) {
        visited_.insert(id);

        if (tryEmitForEach(id)) return;
        if (tryEmitWhile(id))   return;

        const BcBasicBlock& blk = cfg().block(id);

        if (blk.succs.size() >= 2 && tryEmitIfElse(id)) return;

        // Emit block instructions as statements.
        emitBlock(id);

        // Advance to the next block (fall-through successor).
        if (blk.succs.empty()) break;
        id = blk.succs[0];
        if (id == stopBlock) break;
    }
}

// ─── Method body ─────────────────────────────────────────────────────────────

std::string JavaStmtEmitter::emitBody() {
    out_.writeLine("{");
    out_.indent();

    // Emit local variable declarations (non-params).
    for (const auto& lv : method_.locals) {
        if (lv.isParam) continue;
        std::string typeName = tyPrinter_.print(lv.type);
        out_.writeLine(typeName + " " + lv.name + ";");
    }

    if (!cfg().blocks().empty())
        emitFrom(0);

    out_.dedent();
    out_.writeLine("}");
    return out_.str();
}

} // namespace java_emitter
} // namespace retdec
