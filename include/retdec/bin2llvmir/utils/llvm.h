/**
 * @file include/retdec/bin2llvmir/utils/llvm.h
 * @brief LLVM Utility functions.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 *
 * Useful LLVM-related things that are missing in LLVM itself.
 * All (Values, Types, Instructions, etc.) in one module.
 * Keep this as small as possible. Use LLVM when possible.
 */

#ifndef RETDEC_BIN2LLVMIR_UTILS_LLVM_H
#define RETDEC_BIN2LLVMIR_UTILS_LLVM_H

#include <llvm/ADT/Twine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace retdec {
namespace bin2llvmir {
namespace llvm_utils {

//
//==============================================================================
// Values
//==============================================================================
//

llvm::Value* skipCasts(llvm::Value* val);

//
//==============================================================================
// Types
//==============================================================================
//

llvm::IntegerType* getCharType(llvm::LLVMContext& ctx);
llvm::PointerType* getCharPointerType(llvm::LLVMContext& ctx);

bool isCharType(const llvm::Type* t);
bool isCharPointerType(const llvm::Type* t);
bool isStringArrayType(const llvm::Type* t);
bool isStringArrayPointeType(const llvm::Type* t);

llvm::Type* stringToLlvmType(llvm::LLVMContext& ctx, const std::string& str);
llvm::Type* stringToLlvmTypeDefault(llvm::Module* m, const std::string& str);

/// RetDec pointee type on an instruction. Kind `retdec.pointee`.
/// Same attachment pattern as `insn.addr`. Opaque LLVM `ptr` has no element
/// type; load/store and type recovery read this metadata.
void setPointeeTypeMetadata(llvm::Instruction* i, llvm::Type* pointee);
llvm::Type* getPointeeTypeMetadata(const llvm::Instruction* i);
/// Metadata first, then alloca allocated type / global value type,
/// then bitcast/select/PHI operands, then load/store users (LLVM 23
/// `ConstantData` has no use-list — those are skipped).
llvm::Type* pointeeType(const llvm::Value* v);
/// Opaque `ptr` has no Type* element. Always null; use `pointeeType(Value*)`.
llvm::Type* typedPointerElement(const llvm::Type* t);

/// Struct / array / fixed-vector element at `idx`. Null if `t` is not aggregate.
llvm::Type* aggregateTypeAtIndex(llvm::Type* t, unsigned idx);

/// Typed load: LLVM 23 `LoadInst` requires an explicit type. Stamps `retdec.pointee`.
llvm::LoadInst* createLoadInst(
		llvm::Value* ptr,
		llvm::Type* ty,
		const llvm::Twine& name = "",
		llvm::Instruction* insertBefore = nullptr);
llvm::LoadInst* createLoadInst(
		llvm::Value* ptr,
		const llvm::Twine& name = "",
		llvm::Instruction* insertBefore = nullptr);

llvm::StoreInst* createStoreInst(
		llvm::Value* val,
		llvm::Value* ptr,
		llvm::Instruction* insertBefore = nullptr);

std::vector<llvm::Type*>
parseFormatString(llvm::Module* module, const std::string& format, llvm::Function* calledFnc = nullptr);

} // namespace llvm_utils
} // namespace bin2llvmir
} // namespace retdec

#endif
