/**
 * @file src/retdec/llvm_to_ssa.cpp
 * @brief Adapter: build retdec::ssa::SSAModule from an llvm::Module.
 */

#include "llvm_to_ssa.h"
#include "retdec/ssa/ssa.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace retdec {

namespace {

/// Return the human-readable name of a called function, falling back to the
/// mangled LLVM name if demangling is not available.
static std::string calleeName(const llvm::CallInst& ci)
{
	const llvm::Function* f = ci.getCalledFunction();
	if (!f) return ""; // indirect call
	return f->getName().str();
}

/// Map an LLVM binary opcode to the nearest ssa::IrInstr::Op.
static ssa::IrInstr::Op binOp(unsigned llvmOpc)
{
	using Op = ssa::IrInstr::Op;
	switch (llvmOpc)
	{
	case llvm::Instruction::Add:
	case llvm::Instruction::FAdd: return Op::Add;
	case llvm::Instruction::Sub:
	case llvm::Instruction::FSub: return Op::Sub;
	case llvm::Instruction::Mul:
	case llvm::Instruction::FMul: return Op::Mul;
	case llvm::Instruction::SDiv:
	case llvm::Instruction::UDiv:
	case llvm::Instruction::FDiv: return Op::Div;
	case llvm::Instruction::SRem:
	case llvm::Instruction::URem:
	case llvm::Instruction::FRem: return Op::Rem;
	case llvm::Instruction::And: return Op::And;
	case llvm::Instruction::Or: return Op::Or;
	case llvm::Instruction::Xor: return Op::Xor;
	case llvm::Instruction::Shl: return Op::Shl;
	case llvm::Instruction::LShr:
	case llvm::Instruction::AShr: return Op::Shr;
	default: return Op::Assign;
	}
}

/// Address from `insn.addr` (what bin2llvmir writes) or orphan `retdec.addr`.
static uint64_t addressFromMetadata(const llvm::Instruction& li)
{
	auto fromKind = [&](const char* kind) -> uint64_t {
		const llvm::MDNode* md = li.getMetadata(kind);
		if (!md || md->getNumOperands() < 1)
			return 0;
		if (auto* ci = llvm::mdconst::dyn_extract<llvm::ConstantInt>(md->getOperand(0)))
			return ci->getZExtValue();
		return 0;
	};
	if (uint64_t a = fromKind("insn.addr"))
		return a;
	return fromKind("retdec.addr");
}

/// Translate one LLVM instruction into an ssa::IrInstr and append it to
/// the given basic block.  Returns nullptr if the instruction should be
/// skipped (e.g. alloca, getelementptr, unreachable).
static ssa::IrInstr* translateInstr(const llvm::Instruction& li, ssa::SSAFunction& fn, ssa::BasicBlock& blk)
{
	using Op = ssa::IrInstr::Op;

	// Determine VMA: debug-info line is a proxy; real VMA is `insn.addr`.
	uint64_t vma = 0;
	if (const llvm::DebugLoc& loc = li.getDebugLoc())
		vma = loc.getLine();
	if (uint64_t a = addressFromMetadata(li))
		vma = a;

	Op op = Op::Assign;
	std::string calleeStr;

	if (llvm::isa<llvm::CallInst>(li))
	{
		const auto& ci = llvm::cast<llvm::CallInst>(li);
		op = Op::Call;
		calleeStr = calleeName(ci);
	}
	else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&li))
	{
		op = load->isAtomic() ? Op::Lock : Op::Load;
	}
	else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&li))
	{
		op = store->isAtomic() ? Op::Lock : Op::Store;
	}
	else if (llvm::isa<llvm::AtomicRMWInst>(li) || llvm::isa<llvm::AtomicCmpXchgInst>(li)
			 || llvm::isa<llvm::FenceInst>(li))
	{
		op = Op::Lock;
	}
	else if (llvm::isa<llvm::ReturnInst>(li))
	{
		op = Op::Ret;
	}
	else if (const auto* bi = llvm::dyn_cast<llvm::BranchInst>(&li))
	{
		op = bi->isConditional() ? Op::CondBranch : Op::Branch;
	}
	else if (llvm::isa<llvm::ICmpInst>(li) || llvm::isa<llvm::FCmpInst>(li))
	{
		op = Op::Compare;
	}
	else if (const auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&li))
	{
		op = binOp(bo->getOpcode());
	}
	else if (llvm::isa<llvm::PHINode>(li))
	{
		op = Op::Phi;
	}
	else if (
		llvm::isa<llvm::AllocaInst>(li) || llvm::isa<llvm::GetElementPtrInst>(li) || llvm::isa<llvm::BitCastInst>(li)
		|| llvm::isa<llvm::TruncInst>(li) || llvm::isa<llvm::ZExtInst>(li) || llvm::isa<llvm::SExtInst>(li)
		|| llvm::isa<llvm::UnreachableInst>(li))
	{
		return nullptr; // not interesting for analysis passes
	}

	ssa::IrInstr* instr = fn.addInstr(blk.id, op, vma);
	if (instr && !calleeStr.empty()) instr->calleeName = std::move(calleeStr);

	// Detectors (RingBuffer wrap mask, sift-down Shl/Mul imm) read
	// IrInstr::uses. Recovered IR previously left them empty.
	// PHI incoming ConstantInts are the same Immediate form (E6 def-use).
	if (instr
		&& (llvm::isa<llvm::BinaryOperator>(li) || llvm::isa<llvm::PHINode>(li)
			|| llvm::isa<llvm::AtomicRMWInst>(li) || llvm::isa<llvm::AtomicCmpXchgInst>(li)))
	{
		for (unsigned i = 0, n = li.getNumOperands(); i < n; ++i)
		{
			const auto* c = llvm::dyn_cast<llvm::ConstantInt>(li.getOperand(i));
			if (!c || c->getBitWidth() > 64) continue;
			ssa::IrValue* val = fn.allocValue(ssa::ValueKind::Immediate);
			val->imm = c->getZExtValue();
			ssa::Use u;
			u.valueId = val->id;
			u.operandIndex = static_cast<uint8_t>(i);
			instr->uses.push_back(u);
		}
	}

	// For Ret: record the return value as a use so AbiSeeder can find it.
	if (op == Op::Ret && instr)
	{
		if (!li.getOperand(0) || llvm::isa<llvm::UndefValue>(li.getOperand(0)))
		{
			// void return — no use
		}
		else
		{
			// We can't recover full SSA value IDs here without running the
			// full SSA construction pass, so leave uses empty.  The analysis
			// passes that truly need return-value IDs should use the full
			// SSAPass on the output of a proper IR builder.
		}
	}

	return instr;
}

} // anonymous namespace

std::unique_ptr<ssa::SSAModule> buildSsaModule(const llvm::Module& m)
{
	auto mod = std::make_unique<ssa::SSAModule>();

	for (const llvm::Function& lf: m)
	{
		if (lf.isDeclaration()) continue; // external symbol — skip

		ssa::SSAFunction* fn = mod->addFunction(lf.getName().str());

		// Map LLVM basic-block pointer → ssa::BlockId for edge construction.
		std::unordered_map<const llvm::BasicBlock*, ssa::BlockId> bbMap;

		// First pass: create one ssa::BasicBlock per LLVM basic block.
		for (const llvm::BasicBlock& lb: lf)
		{
			ssa::BasicBlock* blk = fn->addBlock(lb.getName().str());
			bbMap[&lb] = blk->id;
		}

		// Second pass: translate instructions and wire CFG edges.
		for (const llvm::BasicBlock& lb: lf)
		{
			ssa::BlockId blkId = bbMap.at(&lb);
			ssa::BasicBlock* blk = fn->block(blkId);
			if (!blk) continue;

			for (const llvm::Instruction& li: lb)
				translateInstr(li, *fn, *blk);

			// Successor edges
			const llvm::Instruction* term = lb.getTerminator();
			if (term)
			{
				for (unsigned i = 0, n = term->getNumSuccessors(); i < n; ++i)
				{
					const llvm::BasicBlock* succ = term->getSuccessor(i);
					auto it = bbMap.find(succ);
					if (it != bbMap.end()) blk->addSucc(it->second);
				}
			}
		}

		// Predecessor edges (reverse of successors)
		for (const llvm::BasicBlock& lb: lf)
		{
			ssa::BlockId blkId = bbMap.at(&lb);
			ssa::BasicBlock* blk = fn->block(blkId);
			if (!blk) continue;
			for (ssa::BlockId succId: blk->succs)
			{
				ssa::BasicBlock* succBlk = fn->block(succId);
				if (succBlk) succBlk->addPred(blkId);
			}
		}
	}

	return mod;
}

} // namespace retdec
