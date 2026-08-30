/**
 * @file src/capstone2llvmir/llvmir_utils.cpp
 * @brief LLVM IR utilities.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <llvm/IR/Metadata.h>
#include <llvm/Support/raw_ostream.h>

#include "capstone2llvmir/llvmir_utils.h"
#include "retdec/capstone2llvmir/exceptions.h"

namespace retdec {
namespace capstone2llvmir {

llvm::Value* generateValueNegate(llvm::IRBuilder<>& irb, llvm::Value* val)
{
	return irb.CreateXor(val, llvm::ConstantInt::getSigned(val->getType(), -1));
}

llvm::IntegerType* getIntegerTypeFromByteSize(llvm::Module* module, unsigned sz)
{
	sz = sz ? 8 * sz : module->getDataLayout().getPointerSizeInBits();
	return llvm::Type::getIntNTy(module->getContext(), sz);
}

void attachPointeeType(llvm::Instruction* i, llvm::Type* pointee)
{
	if (!i || !pointee)
	{
		return;
	}
	std::string printed;
	llvm::raw_string_ostream os(printed);
	pointee->print(os);
	os.flush();
	auto* md = llvm::MDNode::get(i->getContext(), {llvm::MDString::get(i->getContext(), printed)});
	i->setMetadata("retdec.pointee", md);
}

llvm::LoadInst* loadIntPtr(llvm::IRBuilder<>& irb, llvm::Value* addr, llvm::Type* elem, unsigned addrSpace)
{
	auto* pt = llvm::PointerType::get(elem, addrSpace);
	auto* ptr = irb.CreateIntToPtr(addr, pt);
	auto* ld = irb.CreateLoad(elem, ptr);
	if (auto* i = llvm::dyn_cast<llvm::Instruction>(ptr))
	{
		attachPointeeType(i, elem);
	}
	attachPointeeType(ld, elem);
	return ld;
}

llvm::StoreInst*
storeIntPtr(llvm::IRBuilder<>& irb, llvm::Value* val, llvm::Value* addr, llvm::Type* elem, unsigned addrSpace)
{
	if (!elem)
	{
		elem = val->getType();
	}
	auto* pt = llvm::PointerType::get(elem, addrSpace);
	auto* ptr = irb.CreateIntToPtr(addr, pt);
	auto* st = irb.CreateStore(val, ptr);
	if (auto* i = llvm::dyn_cast<llvm::Instruction>(ptr))
	{
		attachPointeeType(i, elem);
	}
	attachPointeeType(st, elem);
	return st;
}

llvm::Value* intToPtr(llvm::IRBuilder<>& irb, llvm::Value* addr, llvm::Type* elem, unsigned addrSpace)
{
	auto* pt = llvm::PointerType::get(elem, addrSpace);
	auto* ptr = irb.CreateIntToPtr(addr, pt);
	if (auto* i = llvm::dyn_cast<llvm::Instruction>(ptr))
	{
		attachPointeeType(i, elem);
	}
	return ptr;
}

llvm::LoadInst* createLoad(llvm::IRBuilder<>& irb, llvm::Value* ptr)
{
	llvm::Type* ty = nullptr;
	llvm::Value* p = ptr;
	while (p)
	{
		if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(p))
		{
			ty = gv->getValueType();
			break;
		}
		if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(p))
		{
			ty = ai->getAllocatedType();
			break;
		}
		if (auto* c = llvm::dyn_cast<llvm::CastInst>(p))
		{
			if (c->getOpcode() == llvm::Instruction::BitCast
				|| c->getOpcode() == llvm::Instruction::AddrSpaceCast)
			{
				p = c->getOperand(0);
				continue;
			}
		}
		break;
	}
	if (!ty)
	{
		throw GenericError("createLoad: missing pointee type");
	}
	auto* ld = irb.CreateLoad(ty, ptr);
	attachPointeeType(ld, ty);
	return ld;
}

llvm::Type* getFloatTypeFromByteSize(llvm::Module* module, unsigned sz)
{
	auto& ctx = module->getContext();
	switch (sz)
	{
	case 2: return llvm::Type::getHalfTy(ctx);
	case 4: return llvm::Type::getFloatTy(ctx);
	case 8: return llvm::Type::getDoubleTy(ctx);
	case 10: return llvm::Type::getX86_FP80Ty(ctx);
	case 16: return llvm::Type::getFP128Ty(ctx);
	default: throw GenericError("Unhandled value in getFloatTypeFromByteSize()."); return llvm::Type::getFloatTy(ctx);
	}
}

llvm::Instruction* _generateIfThen(llvm::Value* cond, llvm::IRBuilder<>& irb, bool reverse)
{
	if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cond))
	{
		if (ci->isZero())
		{
			if (reverse)
			{
				// llvm::BranchInst::Create(after, body, cond, ipBb->getTerminator());
				// cond == false -> never jump to after -> body always executed
				if (irb.GetInsertPoint() == irb.GetInsertBlock()->end())
				{
					throw GenericError("Bad insert point in _generateIfThen().");
				}
				return &*irb.GetInsertPoint();
			}
			else
			{
				// llvm::BranchInst::Create(body, after, cond, ipBb->getTerminator());
				// todo: cond == false -> never jump to body -> body never executed
			}
		}
		else
		{
			if (reverse)
			{
				// llvm::BranchInst::Create(after, body, cond, ipBb->getTerminator());
				// todo: cond == true -> always jump to after -> body never executed
			}
			else
			{
				// llvm::BranchInst::Create(body, after, cond, ipBb->getTerminator());
				// cond == true -> always jump to body -> body always executed
				if (irb.GetInsertPoint() == irb.GetInsertBlock()->end())
				{
					throw GenericError("Bad insert point in _generateIfThen().");
				}
				return &*irb.GetInsertPoint();
			}
		}
	}

	auto* ipBb = irb.GetInsertBlock();
	auto ipIt = irb.GetInsertPoint();
	if (ipIt == ipBb->end())
	{
		throw GenericError("Bad insert point in _generateIfThen().");
	}
	llvm::Instruction* ip = &(*ipIt);

	auto* body = ipBb->splitBasicBlock(ip);
	auto* after = body->splitBasicBlock(ip);

	if (reverse)
	{
		llvm::BranchInst::Create(after, body, cond, ipBb->getTerminator());
	}
	else
	{
		llvm::BranchInst::Create(body, after, cond, ipBb->getTerminator());
	}
	ipBb->getTerminator()->eraseFromParent();
	irb.SetInsertPoint(ip);

	return body->getTerminator();
}

llvm::Instruction* generateIfThen(llvm::Value* cond, llvm::IRBuilder<>& irb)
{
	return _generateIfThen(cond, irb, false);
}

llvm::Instruction* generateIfNotThen(llvm::Value* cond, llvm::IRBuilder<>& irb)
{
	return _generateIfThen(cond, irb, true);
}

std::pair<llvm::Instruction*, llvm::Instruction*> generateIfThenElse(llvm::Value* cond, llvm::IRBuilder<>& irb)
{
	auto* ipBb = irb.GetInsertBlock();
	auto ipIt = irb.GetInsertPoint();
	if (ipIt == ipBb->end())
	{
		throw GenericError("Bad insert point in _generateIfThen().");
	}
	llvm::Instruction* ip = &(*ipIt);

	auto* bodyIf = ipBb->splitBasicBlock(ip);
	auto* bodyElse = bodyIf->splitBasicBlock(ip);
	auto* after = bodyElse->splitBasicBlock(ip);

	llvm::BranchInst::Create(bodyIf, bodyElse, cond, ipBb->getTerminator());
	ipBb->getTerminator()->eraseFromParent();

	llvm::BranchInst::Create(after, bodyIf->getTerminator());
	bodyIf->getTerminator()->eraseFromParent();

	irb.SetInsertPoint(ip);

	return {bodyIf->getTerminator(), bodyElse->getTerminator()};
}

std::pair<llvm::Instruction*, llvm::Instruction*> generateWhile(llvm::BranchInst*& branch, llvm::IRBuilder<>& irb)
{
	auto* ipBb = irb.GetInsertBlock();
	auto ipIt = irb.GetInsertPoint();
	if (ipIt == ipBb->end())
	{
		throw GenericError("Bad insert point in _generateIfThen().");
	}
	llvm::Instruction* ip = &(*ipIt);

	auto* before = ipBb->splitBasicBlock(ip);
	auto* body = before->splitBasicBlock(ip);
	auto* after = body->splitBasicBlock(ip);

	branch = llvm::BranchInst::Create(body, after, irb.getTrue(), before->getTerminator());
	before->getTerminator()->eraseFromParent();

	llvm::BranchInst::Create(before, body->getTerminator());
	body->getTerminator()->eraseFromParent();

	irb.SetInsertPoint(ip);

	return {before->getTerminator(), body->getTerminator()};
}

} // namespace capstone2llvmir
} // namespace retdec
