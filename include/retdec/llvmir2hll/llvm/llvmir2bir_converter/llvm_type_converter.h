/**
* @file include/retdec/llvmir2hll/llvm/llvmir2bir_converter/llvm_type_converter.h
* @brief A converter from LLVM type to type in BIR.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_LLVM_LLVMIR2BIR_CONVERTER_LLVM_TYPE_CONVERTER_H
#define RETDEC_LLVMIR2HLL_LLVM_LLVMIR2BIR_CONVERTER_LLVM_TYPE_CONVERTER_H

#include <unordered_map>

#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/utils/non_copyable.h"

namespace llvm {

class ArrayType;
class FunctionType;
class IntegerType;
class PointerType;
class StructType;
class Type;

} // namespace llvm

namespace retdec {
namespace llvmir2hll {

class ArrayType;
class FunctionType;
class IntType;
class PointerType;
class StructType;
class Type;

/**
* @brief A converter from LLVM type to type in BIR.
*/
class LLVMTypeConverter final: private retdec::utils::NonCopyable {
public:
	LLVMTypeConverter();

	bool isBool(const llvm::IntegerType *type) const;

	ShPtr<Type> convert(const llvm::Type *type);
	ShPtr<PointerType> convert(const llvm::PointerType *type);
	ShPtr<ArrayType> convert(const llvm::ArrayType *type);
	ShPtr<StructType> convert(const llvm::StructType *type);
	ShPtr<FunctionType> convert(const llvm::FunctionType *type);

private:
	/// Converts an LLVM integer type, tagging standard widths (8/16/32/64)
	/// as unsigned so the HLL writer can emit uint8_t / uint16_t / etc.
	ShPtr<IntType> convertIntegerType(const llvm::IntegerType *type);

	/// Mapping of an LLVM type into an already converted type in BIR.
	std::unordered_map<const llvm::Type *, ShPtr<Type>> mapLLVMTypeToType;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
