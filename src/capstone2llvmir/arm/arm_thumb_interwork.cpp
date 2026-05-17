/**
* @file src/capstone2llvmir/arm/arm_thumb_interwork.cpp
* @brief ARM Thumb↔ARM interworking: annotate BX/BLX mode switches.
* @copyright (c) 2024, MIT license
*
* The ARM lifter in capstone2llvmir correctly identifies BX/BLX instructions
* but does not record that the target might be in a different ISA mode
* (Thumb vs ARM), which causes two problems:
*
*  1. The decoder continues lifting in the current mode after a BX LR that
*     returns into a Thumb caller, producing garbage IR for the next function.
*
*  2. BLX to a Thumb function is emitted as a plain call with no indication
*     that the callee is Thumb — the called function's body may already be
*     lifted in Thumb mode, but the call site has no metadata to confirm this.
*
* This file provides:
*
*   annotateThumbInterwork(CallInst* call, bool targetIsThumb)
*     — attaches "arm.thumb_call" i1 metadata to the call instruction.
*
*   isThumbAddress(uint64_t addr)
*     — returns true if the low bit of @a addr is set (Thumb interwork bit).
*
*   patchBxBlxCalls(Module& m, Config* config)
*     — walks all calls emitted for BX/BLX instructions and attaches
*       the interwork metadata based on the target address's low bit.
*
* Call patchBxBlxCalls() from the ARM capstone2llvmir translation unit after
* all instructions in a function have been lifted (post-lift hook).
*/

#include <cstdint>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/utils/io/log.h"

using namespace llvm;
using namespace retdec::utils::io;

namespace retdec {
namespace capstone2llvmir {

bool isThumbAddress(uint64_t addr) {
    return (addr & 1) != 0;
}

void annotateThumbInterwork(CallInst* call, bool targetIsThumb) {
    LLVMContext& ctx = call->getContext();
    auto* flag = ConstantInt::get(Type::getInt1Ty(ctx),
                                   targetIsThumb ? 1 : 0);
    auto* mdn = MDNode::get(ctx, {ValueAsMetadata::get(flag)});
    call->setMetadata("arm.thumb_call", mdn);
}

/// Patch all BX/BLX call instructions in @a fn with interwork metadata.
/// A BX/BLX instruction is identified by the "insn.id" metadata that the
/// ARM lifter attaches (ARM_INS_BX = 14, ARM_INS_BLX = 13 in capstone numbering).
static void patchFunction(Function& fn, bin2llvmir::Config* config) {
    for (auto& bb : fn) {
        for (auto& inst : bb) {
            auto* call = dyn_cast<CallInst>(&inst);
            if (!call) continue;

            // Check for BX/BLX via metadata.
            auto* idMdn = inst.getMetadata("insn.id");
            if (!idMdn) continue;
            auto* idCI = mdconst::dyn_extract<ConstantInt>(idMdn->getOperand(0));
            if (!idCI) continue;

            uint64_t insnId = idCI->getZExtValue();
            // capstone ARM_INS_BX = 14, ARM_INS_BLX = 13
            constexpr uint64_t ARM_INS_BX  = 14;
            constexpr uint64_t ARM_INS_BLX = 13;
            if (insnId != ARM_INS_BX && insnId != ARM_INS_BLX) continue;

            // Try to determine target address from the call's operand.
            // The target may be a constant (direct BLX #addr) or a register
            // (indirect BX Rm).
            bool targetIsThumb = false;

            if (auto* ci = dyn_cast<ConstantInt>(call->getArgOperand(0))) {
                uint64_t targetAddr = ci->getZExtValue();
                targetIsThumb = isThumbAddress(targetAddr);
            } else {
                // Indirect — we can't determine statically. Default to
                // annotating as potentially-Thumb (conservative).
                targetIsThumb = true;
            }

            annotateThumbInterwork(call, targetIsThumb);

            Log::info() << "[ThumbInterwork] " << fn.getName().str()
                        << ": BX/BLX → " << (targetIsThumb ? "Thumb" : "ARM") << "\n";
        }
    }
}

void patchBxBlxCalls(Module& m, bin2llvmir::Config* config) {
    for (auto& fn : m)
        if (!fn.isDeclaration())
            patchFunction(fn, config);
}

} // namespace capstone2llvmir
} // namespace retdec
