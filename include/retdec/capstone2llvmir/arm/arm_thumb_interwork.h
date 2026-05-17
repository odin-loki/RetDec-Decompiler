/**
* @file include/retdec/capstone2llvmir/arm/arm_thumb_interwork.h
* @brief ARM Thumb↔ARM interworking annotations.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_CAPSTONE2LLVMIR_ARM_THUMB_INTERWORK_H
#define RETDEC_CAPSTONE2LLVMIR_ARM_THUMB_INTERWORK_H

#include <cstdint>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace retdec {
namespace bin2llvmir { class Config; }
namespace capstone2llvmir {

/// Returns true if @a addr has the Thumb interwork bit set (bit 0 = 1).
bool isThumbAddress(uint64_t addr);

/// Attach "arm.thumb_call" i1 metadata to @a call.
void annotateThumbInterwork(llvm::CallInst* call, bool targetIsThumb);

/// Walk all BX/BLX calls in @a m and attach interwork metadata.
void patchBxBlxCalls(llvm::Module& m, bin2llvmir::Config* config);

} // namespace capstone2llvmir
} // namespace retdec

#endif
