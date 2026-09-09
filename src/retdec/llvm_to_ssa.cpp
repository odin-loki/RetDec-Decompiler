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

#include <unordered_map>
#include <utility>
#include <vector>

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

/// The SSA value kind an LLVM value maps to when it is defined by an
/// instruction.  A phi is its own kind so the version counters stay separate;
/// everything else is a virtual register.
static ssa::ValueKind kindOfDef(const llvm::Value& v)
{
	return llvm::isa<llvm::PHINode>(v) ? ssa::ValueKind::Phi : ssa::ValueKind::VirtualReg;
}

/// Width in bits, where the type says one, so a detector can tell a byte from a
/// quadword.  0 means "not an integer", which leaves IrValue::width at its
/// default.
static uint8_t widthOf(const llvm::Value& v)
{
	if (const auto* it = llvm::dyn_cast<llvm::IntegerType>(v.getType()))
	{
		const unsigned bits = it->getBitWidth();
		if (bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128) return static_cast<uint8_t>(bits);
	}
	return 0;
}

/// True for operands that are not values in the def-use sense: the block
/// labels of a branch, and the callee of a direct call.  Treating a label as a
/// use would put a block id into the value space.
static bool isNonValueOperand(const llvm::Value* op)
{
	return op == nullptr || llvm::isa<llvm::BasicBlock>(op) || llvm::isa<llvm::Function>(op);
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
	else if (llvm::isa<llvm::SelectInst>(li))
	{
		// A select consumes a comparison result and produces one of two
		// values, which is exactly what Op::FlagRead names (SETcc, CMOVcc).
		// It used to match no branch here and arrive as Op::Undef, which left
		// the detectors with no way to tell a loop that *selects* its
		// accumulator -- max/min -- from one that combines it.
		op = Op::FlagRead;
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

	// defValue and uses are filled in by buildSsaModule, which is the only
	// place that has the whole function in hand. A use has to name the value
	// its defining instruction defines, and that definition may not have been
	// translated yet -- a phi at a loop header names a value from the latch --
	// so operands cannot be resolved one instruction at a time.

	// For Ret: record the return value as a use so AbiSeeder can find it.
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

		// Second pass: give every value an id, before any instruction is
		// translated.
		//
		// This is what makes the graph a graph. A use has to name the value its
		// defining instruction defines, and definitions are not in use order --
		// a phi at a loop header names a value the latch defines further down.
		// So ids are handed out for the whole function first, and operands are
		// resolved against that map afterwards.
		//
		// Ids are allocated for instructions this adapter does not translate
		// too -- alloca, getelementptr, the casts -- because they still stand
		// between a definition and its use. Skipping them would break the chain
		// at every `load` through a gep, which is most of them.
		std::unordered_map<const llvm::Value*, ssa::ValueId> valueMap;
		const auto valueIdFor = [&](const llvm::Value& v, ssa::ValueKind kind) {
			auto it = valueMap.find(&v);
			if (it != valueMap.end()) return it->second;
			ssa::IrValue* val = fn->allocValue(kind);
			if (const uint8_t w = widthOf(v)) val->width = w;
			valueMap[&v] = val->id;
			return val->id;
		};

		// Arguments are definitions too: without them the first use of a
		// parameter resolves to nothing.
		for (const llvm::Argument& arg: lf.args())
			valueIdFor(arg, ssa::ValueKind::VirtualReg);

		for (const llvm::BasicBlock& lb: lf)
			for (const llvm::Instruction& li: lb)
				if (!li.getType()->isVoidTy()) valueIdFor(li, kindOfDef(li));

		// Third pass: translate instructions and wire CFG edges, remembering
		// which IrInstr each LLVM instruction became so the operands can be
		// resolved once the whole function has ids.
		std::vector<std::pair<const llvm::Instruction*, ssa::IrInstr*>> translated;
		for (const llvm::BasicBlock& lb: lf)
		{
			ssa::BlockId blkId = bbMap.at(&lb);
			ssa::BasicBlock* blk = fn->block(blkId);
			if (!blk) continue;

			for (const llvm::Instruction& li: lb)
			{
				ssa::IrInstr* instr = translateInstr(li, *fn, *blk);
				if (!instr) continue;
				if (!li.getType()->isVoidTy())
				{
					auto it = valueMap.find(&li);
					if (it != valueMap.end()) instr->defValue = it->second;
				}
				translated.push_back({&li, instr});
			}

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

		// Fourth pass: every operand becomes a use.
		//
		// This used to run only for four instruction classes and only for
		// ConstantInt operands, with a `continue` dropping everything else --
		// so a register operand, which is the entire point of a def-use graph,
		// was never a use, and Load and Store were not in the four classes at
		// all and arrived with empty use lists. Any predicate that followed a
		// non-immediate use was dead on production input, whatever the code it
		// was looking at; the detector unit tests passed only because their
		// fixtures hand-build the operand lists this adapter did not deliver.
		for (const auto& [li, instr]: translated)
		{
			for (unsigned i = 0, n = li->getNumOperands(); i < n; ++i)
			{
				const llvm::Value* op = li->getOperand(i);
				if (isNonValueOperand(op)) continue;

				ssa::ValueId id = ssa::kInvalidValue;
				if (const auto* c = llvm::dyn_cast<llvm::ConstantInt>(op))
				{
					if (c->getBitWidth() > 64) continue;
					// A constant is a fresh Immediate each time it appears:
					// two `add x, 1` do not share a value, and the detectors
					// that read `uses[k].imm` want the operand's own entry.
					ssa::IrValue* val = fn->allocValue(ssa::ValueKind::Immediate);
					val->imm = c->getZExtValue();
					if (const uint8_t w = widthOf(*op)) val->width = w;
					id = val->id;
				}
				else
				{
					// A definition this function has an id for, or something
					// from outside it -- a global, a constant expression -- for
					// which one id is minted and shared by every reference.
					id = valueIdFor(*op, ssa::ValueKind::VirtualReg);
				}

				ssa::Use u;
				u.valueId = id;
				// operandIndex is a uint8_t; a call with more than 255
				// arguments would wrap it, so it saturates instead.
				u.operandIndex = static_cast<uint8_t>(i > 255u ? 255u : i);
				instr->uses.push_back(u);
			}
		}
	}

	return mod;
}

} // namespace retdec
