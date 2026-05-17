/**
* @file include/retdec/llvmir2hll/llvm/llvm_intrinsic_converter_ext.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_LLVM_INTRINSIC_CONVERTER_EXT_H
#define RETDEC_LLVM_INTRINSIC_CONVERTER_EXT_H

#include <string>

namespace retdec {
namespace llvmir2hll {

bool isStrippableLLVMIntrinsic(const std::string& funcName);
std::string getExtendedIntrinsicName(const std::string& funcName);

} // namespace llvmir2hll
} // namespace retdec

#endif
