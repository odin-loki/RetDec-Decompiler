/**
* @file src/llvmir2hll/llvm/llvmir2bir_converter/llvm_type_converter.cpp
* @brief Implementation of LLVMTypeConverter.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include "retdec/llvmir2hll/ir/array_type.h"
#include "retdec/llvmir2hll/ir/float_type.h"
#include "retdec/llvmir2hll/ir/function_type.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/struct_type.h"
#include "retdec/llvmir2hll/ir/unknown_type.h"
#include "retdec/llvmir2hll/ir/void_type.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converter/llvm_type_converter.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

namespace {

/// Bit widths that map to the standard C99/C11 fixed-width integer types
/// (uint8_t, uint16_t, uint32_t, uint64_t / their signed counterparts).
/// Any other width is represented as a plain IntType with the exact bit count
/// so that the HLL writer can still choose the best approximation.
constexpr unsigned STDINT_WIDTHS[] = {8, 16, 32, 64};

bool isStandardWidth(unsigned bits) {
	for (auto w : STDINT_WIDTHS) {
		if (bits == w) return true;
	}
	return false;
}

} // anonymous namespace

/**
* @brief Constructs a new converter.
*/
LLVMTypeConverter::LLVMTypeConverter(): mapLLVMTypeToType() {}

/**
* @brief Determines whether LLVM integral type @a type is boolean.
*
* @par Preconditions
*  - @a type is non-null
*/
bool LLVMTypeConverter::isBool(const llvm::IntegerType *type) const {
	PRECONDITION_NON_NULL(type);

	return type->getBitWidth() == 1;
}

/**
* @brief Converts an LLVM integer type to a BIR integer type.
*
* Standard-width integers (8, 16, 32, 64 bits) are tagged as unsigned by
* default because LLVM IR integer types are inherently signless; signedness is
* inferred later by the instruction converter from the operations applied to
* each value (e.g. zext → unsigned, sext → signed, sdiv → signed operands).
* Preserving the exact bit width here ensures that the C emitter can render
* them as uint8_t / uint16_t / uint32_t / uint64_t rather than a generic
* "int".
*
* Non-standard widths (e.g. i24, i48) fall through to plain IntType with the
* exact bit count; the C emitter will approximate these as the next wider
* standard type.
*
* @par Preconditions
*  - @a type is non-null
*/
ShPtr<IntType> LLVMTypeConverter::convertIntegerType(
		const llvm::IntegerType *type) {
	PRECONDITION_NON_NULL(type);

	unsigned bits = type->getBitWidth();

	// Boolean is handled at a higher level; it must not reach here as an
	// integer type.
	ASSERT(!isBool(type));

	// For standard widths, emit an unsigned IntType so the HLL writer can
	// choose the appropriate fixed-width C type (uint8_t etc.).  Signedness
	// is corrected later by the instruction converter when it observes the
	// operations applied to the value.
	if (isStandardWidth(bits)) {
		return IntType::create(bits, /* isSigned */ false);
	}

	// For non-standard widths keep the existing behaviour: plain IntType
	// with the exact bit count.
	return IntType::create(bits);
}

/**
* @brief Converts the given LLVM type @a type into a type in BIR.
*
* @par Preconditions
*  - @a type is non-null
*/
ShPtr<Type> LLVMTypeConverter::convert(const llvm::Type *type) {
	PRECONDITION_NON_NULL(type);

	auto existingTypeIt = mapLLVMTypeToType.find(type);
	if (existingTypeIt != mapLLVMTypeToType.end()) {
		return existingTypeIt->second;
	}

	ShPtr<Type> birType;
	if (type->isIntegerTy()) {
		auto llvmIntType = llvm::cast<llvm::IntegerType>(type);
		if (isBool(llvmIntType)) {
			// i1 → BIR bool; the IntType path is not used for booleans.
			birType = IntType::create(1);
		} else {
			birType = convertIntegerType(llvmIntType);
		}
	} else if (type->isFloatingPointTy()) {
		birType = FloatType::create(type->getPrimitiveSizeInBits());
	} else if (type->isArrayTy()) {
		auto llvmArrayType = llvm::cast<llvm::ArrayType>(type);
		birType = convert(llvmArrayType);
	} else if (type->isStructTy()) {
		auto llvmStructType = llvm::cast<llvm::StructType>(type);
		birType = convert(llvmStructType);
	} else if (type->isPointerTy()) {
		auto llvmPtrType = llvm::cast<llvm::PointerType>(type);
		birType = convert(llvmPtrType);
	} else if (type->isFunctionTy()) {
		auto llvmFuncType = llvm::cast<llvm::FunctionType>(type);
		birType = convert(llvmFuncType);
	} else if (type->isVoidTy()) {
		birType = VoidType::create();
	} else if (type->isVectorTy()) {
		// LLVM vector types (e.g. <4 x i32>) are not natively representable
		// in the HLL BIR.  Map them to an unsigned integer of the same total
		// bit-width, clamped to 64 bits so the HLL writer always emits a
		// standard C type.  Semantic precision is sacrificed here; the
		// entry_alloca pass is responsible for lowering vector ops to scalar
		// arithmetic before llvmir2hll sees them.
		unsigned totalBits = type->getPrimitiveSizeInBits();
		if (totalBits == 0 || totalBits > 64) {
			totalBits = 64;
		}
		birType = IntType::create(totalBits, false);
	} else {
		FAIL("unsupported type: " << const_cast<llvm::Type &>(*type));
		// Fallback in Release builds where FAIL is a no-op: use i64 to
		// prevent null-pointer dereferences in callers.
		birType = IntType::create(64, false);
	}

	// We need to store the converted type to prevent looping when converting
	// recursive types (containing pointers to the currently converted type).
	mapLLVMTypeToType.emplace(type, birType);

	return birType;
}

/**
* @brief Converts the given LLVM pointer type @a type into a pointer type in BIR.
*
* @par Preconditions
*  - @a type is non-null
*/
ShPtr<PointerType> LLVMTypeConverter::convert(const llvm::PointerType *type) {
	PRECONDITION_NON_NULL(type);

	auto birType = PointerType::create(UnknownType::create());
	mapLLVMTypeToType.emplace(type, birType);

	birType->setContainedType(convert(type->getElementType()));
	return birType;
}

/**
* @brief Converts the given LLVM array type @a type into a array type in BIR.
*
* @par Preconditions
*  - @a type is non-null
*/
ShPtr<ArrayType> LLVMTypeConverter::convert(const llvm::ArrayType *type) {
	PRECONDITION_NON_NULL(type);

	ArrayType::Dimensions arrayDims = {static_cast<std::size_t>(type->getNumElements())};

	auto elemTypeIt = type->getElementType();
	while (auto elemArrayType = llvm::dyn_cast<llvm::ArrayType>(elemTypeIt)) {
		arrayDims.push_back(elemArrayType->getNumElements());
		elemTypeIt = elemArrayType->getElementType();
	}

	auto elemType = convert(elemTypeIt);
	return ArrayType::create(elemType, arrayDims);
}

/**
* @brief Converts the given LLVM struct type @a type into a struct type in BIR.
*
* @par Preconditions
*  - @a type is non-null
*/
ShPtr<StructType> LLVMTypeConverter::convert(const llvm::StructType *type) {
	PRECONDITION_NON_NULL(type);

	StructType::ElementTypes elemTypes;
	for (const auto &elem: type->elements()) {
		elemTypes.push_back(convert(elem));
	}

	std::string name = type->hasName() ? type->getName() : "";
	return StructType::create(elemTypes, name);
}

/**
* @brief Converts the given LLVM function type @a type into a function type in BIR.
*
* @par Preconditions
*  - @a type is non-null
*/
ShPtr<FunctionType> LLVMTypeConverter::convert(const llvm::FunctionType *type) {
	PRECONDITION_NON_NULL(type);

	auto retType = convert(type->getReturnType());
	auto funcType = FunctionType::create(retType);

	for (const auto &argType: type->params()) {
		funcType->addParam(convert(argType));
	}

	if (type->isVarArg()) {
		funcType->setVarArg();
	}

	return funcType;
}

} // namespace llvmir2hll
} // namespace retdec
