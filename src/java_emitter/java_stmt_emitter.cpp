/**
 * @file src/java_emitter/java_stmt_emitter.cpp
 * @brief Java statement emitter — walks BcCFG and emits Java statements.
 */

#include "retdec/java_emitter/java_stmt_emitter.h"

#include <algorithm>
#include <map>
#include <cassert>

namespace retdec {
namespace java_emitter {

using namespace bc_module;
using namespace jvm_reconstruct;

// ─── CodeWriter ──────────────────────────────────────────────────────────────

CodeWriter::CodeWriter(int indentWidth): level_(0), indentWidth_(indentWidth)
{
	updateIndentStr();
}

void CodeWriter::updateIndentStr()
{
	indentStr_ = std::string(static_cast<size_t>(level_ * indentWidth_), ' ');
}

void CodeWriter::indent()
{
	++level_;
	updateIndentStr();
}
void CodeWriter::dedent()
{
	if (level_ > 0)
	{
		--level_;
		updateIndentStr();
	}
}

void CodeWriter::writeLine(const std::string& line)
{
	if (line.empty())
	{
		buf_ << "\n";
	}
	else
	{
		buf_ << indentStr_ << line << "\n";
	}
}

void CodeWriter::writeLine()
{
	buf_ << "\n";
}

void CodeWriter::write(const std::string& s)
{
	buf_ << s;
}

std::string CodeWriter::str() const
{
	return buf_.str();
}

// ─── JavaStmtEmitter ─────────────────────────────────────────────────────────

JavaStmtEmitter::JavaStmtEmitter(
	const BcMethod& method,
	const ReconstructResult& recon,
	const JavaTypePrinter& tyPrinter,
	const StmtEmitOptions& opts):
	method_(method), recon_(recon), tyPrinter_(tyPrinter), opts_(opts), exprCtx_(method, recon, tyPrinter),
	exprEmit_(exprCtx_)
{
	buildPatternMaps();
	buildExceptionRegions();
}

void JavaStmtEmitter::buildPatternMaps()
{
	for (size_t i = 0; i < recon_.patterns.forEachLoops.size(); ++i)
		forEachByHeader_[recon_.patterns.forEachLoops[i].loopHeaderBlock] = i;
	for (size_t i = 0; i < recon_.patterns.stringConcats.size(); ++i)
		stringConcatByBlock_[recon_.patterns.stringConcats[i].blockId] = i;
	for (size_t i = 0; i < recon_.patterns.lambdas.size(); ++i)
		lambdaByBlock_[recon_.patterns.lambdas[i].blockId] = i;
}

/**
 * Map each exception handler onto the blocks it protects.
 *
 * A BcExceptionHandler names its protected region in BYTECODE OFFSETS and its
 * handler as a BLOCK INDEX. The tryEmitTryCatch this replaces compared the two
 * directly -- `cfg().block(blockId).id >= eh.startOffset`, a block index
 * against an offset -- so with any handler present at all it claimed almost
 * every block. A block is protected here when its first instruction's offset
 * falls inside the half-open range, which is what the range means.
 *
 * Handlers protecting the same range are the clauses of one try, kept in table
 * order: that is the order the runtime tests them, and so the source order of
 * the catch clauses. Regions are ordered widest-first at a shared start block,
 * which is what makes a nested try come out nested rather than as a sibling.
 */
void JavaStmtEmitter::buildExceptionRegions()
{
	const auto& handlers = cfg().handlers();
	if (handlers.empty()) return;

	auto blockOffset = [this](uint32_t id) -> uint32_t {
		const BcBasicBlock& blk = cfg().block(id);
		return blk.instrs.empty() ? UINT32_MAX : blk.instrs.front().offset;
	};

	std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> byRange;
	for (size_t i = 0; i < handlers.size(); ++i)
	{
		const auto& eh = handlers[i];
		if (eh.handlerBlock >= cfg().blockCount()) continue;
		if (eh.endOffset <= eh.startOffset) continue;
		handlerEntryBlocks_.insert(eh.handlerBlock);
		byRange[{eh.startOffset, eh.endOffset}].push_back(i);
	}

	for (const auto& entry: byRange)
	{
		const uint32_t rangeStart = entry.first.first;
		const uint32_t rangeEnd = entry.first.second;

		TryRegion region;
		region.handlerIndices = entry.second;

		// A handler's own block is never part of the region it protects,
		// whatever the offsets say: emitting it inside the try would emit the
		// catch body twice.
		for (uint32_t b = 0; b < cfg().blockCount(); ++b)
		{
			const uint32_t off = blockOffset(b);
			if (off == UINT32_MAX) continue;
			if (handlerEntryBlocks_.count(b)) continue;
			if (off >= rangeStart && off < rangeEnd)
			{
				if (region.startBlock == UINT32_MAX) region.startBlock = b;
			}
			else if (off >= rangeEnd && region.startBlock != UINT32_MAX && region.endBlock == UINT32_MAX)
			{
				region.endBlock = b;
			}
		}

		if (region.startBlock == UINT32_MAX) continue; // protects no block
		tryRegions_.push_back(region);
	}

	std::sort(tryRegions_.begin(), tryRegions_.end(), [](const TryRegion& a, const TryRegion& b) {
		if (a.startBlock != b.startBlock) return a.startBlock < b.startBlock;
		return a.endBlock > b.endBlock;
	});
	for (size_t i = 0; i < tryRegions_.size(); ++i)
		tryRegionsByBlock_[tryRegions_[i].startBlock].push_back(i);
}

// ─── CFG helpers ─────────────────────────────────────────────────────────────

bool JavaStmtEmitter::isLoopHeader(uint32_t blockId) const
{
	if (blockId >= cfg().blockCount()) return false;
	return cfg().block(blockId).isLoopHeader;
}

uint32_t JavaStmtEmitter::backEdgeSource(uint32_t blockId) const
{
	if (blockId >= cfg().blockCount()) return UINT32_MAX;
	const BcBasicBlock& hdr = cfg().block(blockId);
	for (uint32_t pred: hdr.preds)
	{
		if (pred >= blockId) // Back edge: pred comes after header in block order.
			return pred;
	}
	return UINT32_MAX;
}

uint32_t JavaStmtEmitter::loopExit(uint32_t blockId) const
{
	if (blockId >= cfg().blockCount()) return UINT32_MAX;
	const BcBasicBlock& hdr = cfg().block(blockId);
	// The exit is the successor not in the loop (the one with higher id than the back-edge src).
	for (uint32_t succ: hdr.succs)
	{
		if (succ > blockId) return succ;
	}
	return UINT32_MAX;
}

uint32_t JavaStmtEmitter::findJoin(uint32_t blockId) const
{
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
	if (a < cfg().blockCount())
	{
		const BcBasicBlock& ablk = cfg().block(a);
		for (uint32_t asucc: ablk.succs)
			if (asucc == b) return b;
	}
	return b; // Best guess.
}

// ─── Block emission ───────────────────────────────────────────────────────────

void JavaStmtEmitter::flushStack(std::vector<ExprNode>& stack)
{
	// Emit any side-effecting expressions left on the stack as statements.
	while (!stack.empty())
	{
		auto node = stack.back();
		stack.pop_back();
		if (node.sideEffects) out_.writeLine(node.text + ";");
	}
}

std::string JavaStmtEmitter::buildCondition(const BcInstruction& branchInsn, std::vector<ExprNode>& stack)
{
	switch (branchInsn.opcode)
	{
	case BcOpcode::IfTrue: {
		if (!stack.empty())
		{
			auto e = stack.back();
			stack.pop_back();
			return e.text;
		}
		return "/* cond */";
	}
	case BcOpcode::IfFalse: {
		if (!stack.empty())
		{
			auto e = stack.back();
			stack.pop_back();
			return "!" + e.text;
		}
		return "/* cond */";
	}
	case BcOpcode::IfEq:
	case BcOpcode::CmpEq: {
		if (stack.size() >= 2)
		{
			auto rhs = stack.back();
			stack.pop_back();
			auto lhs = stack.back();
			stack.pop_back();
			return lhs.text + " == " + rhs.text;
		}
		return "/* == */";
	}
	case BcOpcode::IfNe:
	case BcOpcode::CmpNe: {
		if (stack.size() >= 2)
		{
			auto rhs = stack.back();
			stack.pop_back();
			auto lhs = stack.back();
			stack.pop_back();
			return lhs.text + " != " + rhs.text;
		}
		return "/* != */";
	}
	case BcOpcode::IfLt:
	case BcOpcode::CmpLt: {
		if (stack.size() >= 2)
		{
			auto rhs = stack.back();
			stack.pop_back();
			auto lhs = stack.back();
			stack.pop_back();
			return lhs.text + " < " + rhs.text;
		}
		return "/* < */";
	}
	case BcOpcode::IfGe:
	case BcOpcode::CmpGe: {
		if (stack.size() >= 2)
		{
			auto rhs = stack.back();
			stack.pop_back();
			auto lhs = stack.back();
			stack.pop_back();
			return lhs.text + " >= " + rhs.text;
		}
		return "/* >= */";
	}
	case BcOpcode::IfGt:
	case BcOpcode::CmpGt: {
		if (stack.size() >= 2)
		{
			auto rhs = stack.back();
			stack.pop_back();
			auto lhs = stack.back();
			stack.pop_back();
			return lhs.text + " > " + rhs.text;
		}
		return "/* > */";
	}
	case BcOpcode::IfLe:
	case BcOpcode::CmpLe: {
		if (stack.size() >= 2)
		{
			auto rhs = stack.back();
			stack.pop_back();
			auto lhs = stack.back();
			stack.pop_back();
			return lhs.text + " <= " + rhs.text;
		}
		return "/* <= */";
	}
	default: return "/* cond */";
	}
}

bool JavaStmtEmitter::emitInstrAsStmt(const BcInstruction& insn, std::vector<ExprNode>& exprStack)
{
	std::string expr = exprEmit_.emitInsn(insn, exprStack);
	if (expr.empty()) return false; // Purely internal (dup, swap, etc.)

	// Check if this is a statement-producing instruction.
	switch (insn.opcode)
	{
	case BcOpcode::StoreLocal:
	case BcOpcode::ArrayStore:
	case BcOpcode::PutField:
	case BcOpcode::PutStatic:
	case BcOpcode::Return:
	case BcOpcode::ReturnValue:
	case BcOpcode::Throw:
	case BcOpcode::MonitorEnter:
	case BcOpcode::MonitorExit: out_.writeLine(expr + ";"); return true;

	// Void-returning invocations.
	case BcOpcode::InvokeVirtual:
	case BcOpcode::InvokeInterface:
	case BcOpcode::InvokeSpecial:
	case BcOpcode::InvokeStatic:
	case BcOpcode::InvokeDynamic:
	case BcOpcode::Callvirt:
	case BcOpcode::Call: {
		bool returnsVoid = true;
		if (!insn.operands.empty())
		{
			if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
				returnsVoid = !m->descriptor.returnType || m->descriptor.returnType->isVoid();
		}
		if (returnsVoid)
		{
			out_.writeLine(expr + ";");
			return true;
		}
		return false; // Value pushed to stack.
	}

	default: return false; // Expression result on stack.
	}
}

void JavaStmtEmitter::emitBlock(uint32_t blockId)
{
	if (blockId >= cfg().blockCount()) return;
	const BcBasicBlock& blk = cfg().block(blockId);

	std::vector<ExprNode> exprStack;

	for (size_t i = 0; i < blk.instrs.size(); ++i)
	{
		const auto& insn = blk.instrs[i];

		// Skip branching instructions — handled structurally.
		if (insn.opcode == BcOpcode::Goto || insn.opcode == BcOpcode::IfTrue || insn.opcode == BcOpcode::IfFalse
			|| insn.opcode == BcOpcode::IfEq || insn.opcode == BcOpcode::IfNe || insn.opcode == BcOpcode::IfLt
			|| insn.opcode == BcOpcode::IfGe || insn.opcode == BcOpcode::IfGt || insn.opcode == BcOpcode::IfLe
			|| insn.opcode == BcOpcode::TableSwitch || insn.opcode == BcOpcode::LookupSwitch)
			continue;

		// A "declare the local on its first store" branch stood here, and did
		// three things wrong at once.
		//
		// It computed an ExprNode it never used, whose initialiser was
		// `(exprStack.back(), exprStack.pop_back(), <ternary>)` -- so the POP
		// HAPPENED, taking the value the emitInsn(StoreLocal) on the next line
		// needed off the stack. Every store to a local whose name and type were
		// known came out as
		//
		//     int total = /* stack underflow */;
		//
		// which is what this emitter wrote into decompiled Java. The compiler
		// had been saying so under -Wall the whole time -- "ignoring return
		// value of vector::back(), declared with attribute nodiscard" -- and
		// the fast gate compiles with -Wall and no -Werror, so nothing read it.
		//
		// It also redeclared the variable: emitBody() already writes a
		// declaration for every non-param local before the first block, so the
		// output held both `int total;` and `int total = ...;`. Two
		// declarations of one name in one scope is not Java.
		//
		// emitInstrAsStmt below already emits StoreLocal as `name = value;`,
		// which is the right statement given that declaration.

		emitInstrAsStmt(insn, exprStack);
	}

	flushStack(exprStack);
}

// ─── Structural control-flow emission ────────────────────────────────────────

bool JavaStmtEmitter::tryEmitForEach(uint32_t blockId)
{
	if (!opts_.emitEnhancedFor) return false;
	auto it = forEachByHeader_.find(blockId);
	if (it == forEachByHeader_.end()) return false;
	const ForEachPattern& pat = recon_.patterns.forEachLoops[it->second];

	std::string elemType = tyPrinter_.print(pat.elementType);
	auto nameIt = exprCtx_.localNames.find(pat.elementSlot);
	std::string elemName = (nameIt != exprCtx_.localNames.end()) ? nameIt->second : "item";

	auto collNameIt = exprCtx_.localNames.find(pat.collectionSlot);
	std::string collExpr = (collNameIt != exprCtx_.localNames.end()) ? collNameIt->second : "collection";

	out_.writeLine("for (" + elemType + " " + elemName + " : " + collExpr + ") {");
	out_.indent();
	visited_.insert(blockId);
	emitFrom(pat.bodyBlock, pat.exitBlock);
	out_.dedent();
	out_.writeLine("}");

	visited_.insert(blockId);
	if (pat.exitBlock != UINT32_MAX) emitFrom(pat.exitBlock);
	return true;
}

bool JavaStmtEmitter::tryEmitWhile(uint32_t blockId)
{
	if (!isLoopHeader(blockId)) return false;
	if (forEachByHeader_.count(blockId)) return false; // Handled by for-each.

	uint32_t exitBlock = loopExit(blockId);
	uint32_t backSrc = backEdgeSource(blockId);
	if (backSrc == UINT32_MAX) return false;

	// Build the condition from the header's conditional branch.
	const BcBasicBlock& hdr = cfg().block(blockId);
	std::string cond = "true";
	std::vector<ExprNode> tmpStack;

	for (const auto& insn: hdr.instrs)
	{
		if (insn.opcode == BcOpcode::IfFalse || insn.opcode == BcOpcode::IfTrue)
		{
			cond = buildCondition(insn, tmpStack);
			break;
		}
	}

	out_.writeLine("while (" + cond + ") {");
	out_.indent();
	visited_.insert(blockId);

	// Emit loop body: all blocks until back-edge.
	if (!hdr.succs.empty())
	{
		uint32_t bodyStart = hdr.succs[0];
		if (bodyStart == exitBlock && hdr.succs.size() >= 2) bodyStart = hdr.succs[1];
		emitFrom(bodyStart, blockId);
	}

	out_.dedent();
	out_.writeLine("}");

	if (exitBlock != UINT32_MAX) emitFrom(exitBlock);
	return true;
}

bool JavaStmtEmitter::tryEmitIfElse(uint32_t blockId)
{
	if (blockId >= cfg().blockCount()) return false;
	const BcBasicBlock& blk = cfg().block(blockId);
	if (blk.succs.size() < 2) return false;
	if (isLoopHeader(blockId)) return false;

	uint32_t thenBlock = blk.succs[0];
	uint32_t elseBlock = blk.succs[1];
	uint32_t joinBlock = findJoin(blockId);

	// Build condition.
	std::vector<ExprNode> tmpStack;
	std::string cond = "/* cond */";
	const BcInstruction* branchInsn = nullptr;
	for (auto& insn: blk.instrs)
	{
		if (insn.opcode == BcOpcode::IfTrue || insn.opcode == BcOpcode::IfFalse || insn.opcode == BcOpcode::IfEq
			|| insn.opcode == BcOpcode::IfNe || insn.opcode == BcOpcode::IfLt || insn.opcode == BcOpcode::IfGe
			|| insn.opcode == BcOpcode::IfGt || insn.opcode == BcOpcode::IfLe)
		{
			branchInsn = &insn;
			break;
		}
	}
	if (branchInsn)
	{
		// First emit non-branch instructions to build stack.
		for (const auto& insn: blk.instrs)
		{
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
	if (elseBlock != joinBlock && elseBlock < cfg().blockCount() && !visited_.count(elseBlock))
	{
		out_.writeLine("} else {");
		out_.indent();
		emitFrom(elseBlock, joinBlock);
		out_.dedent();
	}
	out_.writeLine("}");

	if (joinBlock != UINT32_MAX && joinBlock < cfg().blockCount()) emitFrom(joinBlock);
	return true;
}

/**
 * Emit the try statement that starts at @a blockId, if one does.
 *
 * The version this replaces was never called: emitFrom dispatched to
 * tryEmitForEach, tryEmitWhile and tryEmitIfElse and to nothing else, so no
 * method this emitter produced ever contained the word `try`. That mattered
 * more than a missing keyword. The lifters record a handler and mark its entry
 * block (JvmLifter::wireExceptions, DexLifter, CilLifter) but add NO CFG edge
 * to it, so a handler block has no predecessor at all -- and emitFrom walks
 * successors. Nothing reached those blocks and every catch and finally body was
 * silently absent from the decompiled source.
 *
 * @return @c true when a try was emitted, and the caller must not also emit
 *         @a blockId itself
 */
bool JavaStmtEmitter::tryEmitTryCatch(uint32_t blockId)
{
	auto it = tryRegionsByBlock_.find(blockId);
	if (it == tryRegionsByBlock_.end()) return false;

	// Outermost region at this block that has not been opened yet. Reentering
	// through emitProtected below picks up the next one in, so nesting comes
	// out of the ordering rather than out of a special case.
	TryRegion* region = nullptr;
	for (size_t idx: it->second)
	{
		if (!tryRegions_[idx].opened)
		{
			region = &tryRegions_[idx];
			break;
		}
	}
	if (region == nullptr) return false;
	region->opened = true;

	const uint32_t endBlock = region->endBlock;
	const std::vector<size_t> handlerIndices = region->handlerIndices;

	out_.writeLine("try {");
	out_.indent();
	emitProtected(blockId, endBlock);
	out_.dedent();

	const auto& handlers = cfg().handlers();
	for (size_t hi: handlerIndices)
	{
		const auto& eh = handlers[hi];

		if (eh.isFinally)
		{
			out_.writeLine("} finally {");
		}
		else
		{
			// A catch-all that is not a finally is a CLR fault clause, which
			// Java cannot spell; Throwable is the closest thing and the comment
			// says what it was.
			const std::string catchType = eh.catchType.has_value() ? tyPrinter_.print(*eh.catchType) : "Throwable";
			// One name per clause. This used to scan method_.locals for any
			// non-parameter whose name merely CONTAINED "ex" and reuse it for
			// every clause, so two catches in one method declared the same
			// variable -- and a local called "index" was liable to be picked.
			const std::string exVar = "ex" + (catchVarCounter_ == 0 ? std::string() : std::to_string(catchVarCounter_));
			++catchVarCounter_;
			out_.writeLine("} catch (" + catchType + " " + exVar + ") {" + (eh.isFault ? " // fault clause" : ""));
		}

		out_.indent();
		if (eh.handlerBlock < cfg().blockCount() && !visited_.count(eh.handlerBlock))
		{
			const uint32_t previous = emittingHandlerBlock_;
			emittingHandlerBlock_ = eh.handlerBlock;
			emitFrom(eh.handlerBlock);
			emittingHandlerBlock_ = previous;
		}
		out_.dedent();
	}
	out_.writeLine("}");

	// Whatever follows the protected region is outside the try.
	if (endBlock != UINT32_MAX && endBlock < cfg().blockCount()) emitFrom(endBlock);
	return true;
}

/**
 * Emit the body of a protected region: the next try nested at the same block if
 * there is one, otherwise the ordinary block walk.
 */
void JavaStmtEmitter::emitProtected(uint32_t id, uint32_t stopBlock)
{
	if (tryEmitTryCatch(id)) return;
	emitFrom(id, stopBlock);
}

bool JavaStmtEmitter::tryEmitSwitch(uint32_t /*blockId*/)
{
	return false; // Placeholder — full switch requires CFG analysis.
}

bool JavaStmtEmitter::tryEmitDoWhile(uint32_t /*blockId*/)
{
	return false; // Placeholder — do-while requires back-edge analysis.
}

bool JavaStmtEmitter::tryEmitFor(uint32_t /*blockId*/)
{
	return false; // Placeholder — classical for loop recognition.
}

bool JavaStmtEmitter::tryEmitSynchronized(uint32_t /*blockId*/)
{
	return false; // Placeholder — monitor enter/exit pair detection.
}

// ─── emitFrom ────────────────────────────────────────────────────────────────

void JavaStmtEmitter::emitFrom(uint32_t id, uint32_t stopBlock)
{
	while (id < cfg().blockCount() && id != stopBlock && !visited_.count(id))
	{
		// A block that only a catch clause may reach. Falling into it from the
		// ordinary walk would emit the handler body outside its try.
		if (handlerEntryBlocks_.count(id) && id != emittingHandlerBlock_) return;

		// Before visited_, because the try's own body is emitted by re-entering
		// here for the same block: marking it first would make the protected
		// region empty.
		if (tryEmitTryCatch(id)) return;

		visited_.insert(id);

		if (tryEmitForEach(id)) return;
		if (tryEmitWhile(id)) return;

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

std::string JavaStmtEmitter::emitBody()
{
	out_.writeLine("{");
	out_.indent();

	// Emit local variable declarations (non-params).
	for (const auto& lv: method_.locals)
	{
		if (lv.isParam) continue;
		std::string typeName = tyPrinter_.print(lv.type);
		out_.writeLine(typeName + " " + lv.name + ";");
	}

	if (!cfg().blocks().empty()) emitFrom(0);

	out_.dedent();
	out_.writeLine("}");
	return out_.str();
}

} // namespace java_emitter
} // namespace retdec
