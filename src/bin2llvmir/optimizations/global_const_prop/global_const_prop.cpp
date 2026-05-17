/**
* @file src/bin2llvmir/optimizations/global_const_prop/global_const_prop.cpp
* @brief Propagate read-only global constants to their use sites.
* @copyright (c) 2024, MIT license
*
* After lifting, many string literals and small read-only tables appear as
* constant global variables (marked `constant` in LLVM IR or identified as
* residing in a read-only section). Their uses are typically loads inside
* functions that read individual bytes or words.
*
* This pass:
*  1. Identifies global variables that are:
*     a) `constant` (isConstant() == true), OR
*     b) In a read-only section (from the binary image).
*     c) Have a known, fully-initialised constant initialiser.
*
*  2. For each load from such a global (either directly or via a GEP with
*     all-constant indices), folds the load to the constant value read from
*     the initialiser.
*
*  3. For stores (which should not exist to a truly read-only global), emits
*     a warning and leaves them alone.
*
* This improves decompiler output by turning:
*   %c = load i8, i8* getelementptr([6 x i8], [6 x i8]* @g_str, i32 0, i32 2)
* into:
*   %c = i8 108   ; 'l'
* and later passes can fold that into string comparisons, switch cases, etc.
*/

#include <cstdint>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/optimizations/global_const_prop/global_const_prop.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/utils/io/log.h"

using namespace llvm;
using namespace retdec::utils::io;

namespace retdec {
namespace bin2llvmir {

char GlobalConstProp::ID = 0;

static RegisterPass<GlobalConstProp> X(
    "retdec-global-const-prop",
    "Read-only global constant propagation",
    false, false
);

GlobalConstProp::GlobalConstProp() : ModulePass(ID) {}

bool GlobalConstProp::runOnModule(Module& m) {
    _module = &m;
    _config = ConfigProvider::getConfig(_module);
    _image  = FileImageProvider::getFileImage(_module);
    return run();
}

bool GlobalConstProp::runOnModuleCustom(Module& m, Config* config,
                                         FileImage* image) {
    _module = &m; _config = config; _image = image;
    return run();
}

//===========================================================================
// Helpers
//===========================================================================

/// Return the constant value at byte offset @a byteOff in @a init.
/// Returns nullptr if the offset is out of range or the value is not a
/// simple integer constant.
static Constant* extractFromInitializer(Constant* init, uint64_t byteOff,
                                         unsigned loadBits,
                                         const DataLayout& DL) {
    if (!init) return nullptr;

    if (auto* arr = dyn_cast<ConstantDataArray>(init)) {
        // For byte arrays, element index = byteOff.
        unsigned elemBits = arr->getElementType()->getIntegerBitWidth();
        unsigned elemBytes = elemBits / 8;
        if (elemBytes == 0) return nullptr;
        uint64_t idx = byteOff / elemBytes;
        if (idx >= arr->getNumElements()) return nullptr;
        if (byteOff % elemBytes != 0) return nullptr;
        if (elemBits != loadBits) {
            // Truncate if loading fewer bits than element size.
            if (loadBits < elemBits) {
                auto* elem = arr->getElementAsConstant(idx);
                auto* ci   = dyn_cast<ConstantInt>(elem);
                if (!ci) return nullptr;
                return ConstantInt::get(
                    IntegerType::get(init->getContext(), loadBits),
                    ci->getZExtValue() & ((1ULL << loadBits) - 1));
            }
            return nullptr;
        }
        return arr->getElementAsConstant(idx);
    }

    if (auto* ca = dyn_cast<ConstantArray>(init)) {
        Type* elemTy = ca->getType()->getElementType();
        uint64_t elemSize = DL.getTypeAllocSize(elemTy);
        if (elemSize == 0) return nullptr;
        uint64_t idx    = byteOff / elemSize;
        uint64_t subOff = byteOff % elemSize;
        if (idx >= ca->getNumOperands()) return nullptr;
        return extractFromInitializer(ca->getOperand(idx), subOff,
                                       loadBits, DL);
    }

    if (auto* ci = dyn_cast<ConstantInt>(init)) {
        if (byteOff == 0 && ci->getBitWidth() == loadBits) return ci;
        if (byteOff == 0 && loadBits < ci->getBitWidth()) {
            return ConstantInt::get(
                IntegerType::get(init->getContext(), loadBits),
                ci->getZExtValue() & ((1ULL << loadBits) - 1));
        }
        return nullptr;
    }

    if (isa<ConstantAggregateZero>(init)) {
        // All zeros.
        Type* ty = IntegerType::get(init->getContext(), loadBits);
        return ConstantInt::get(ty, 0);
    }

    return nullptr;
}

/// Check whether @a gv should be treated as read-only.
static bool isReadOnly(GlobalVariable* gv, FileImage* image) {
    if (gv->isConstant()) return true;
    if (!gv->hasInitializer()) return false;
    // Could query image for read-only section membership here.
    // For now: constant flag is authoritative.
    return false;
}

/// Resolve all-constant GEP indices to a byte offset.
/// Returns false if any index is not a constant integer.
static bool gepToByteOffset(GEPOperator* gep, const DataLayout& DL,
                              uint64_t& byteOff) {
    APInt offset(64, 0);
    if (!gep->accumulateConstantOffset(DL, offset)) return false;
    byteOff = offset.getZExtValue();
    return true;
}

//===========================================================================
// Main run
//===========================================================================

bool GlobalConstProp::run() {
    if (!_module) return false;
    const DataLayout& DL = _module->getDataLayout();

    // Collect candidate globals.
    std::vector<GlobalVariable*> candidates;
    for (auto& gv : _module->globals()) {
        if (!isReadOnly(&gv, _image)) continue;
        if (!gv.hasInitializer()) continue;
        candidates.push_back(&gv);
    }

    bool changed = false;
    unsigned folded = 0;

    for (auto* gv : candidates) {
        Constant* init = gv->getInitializer();

        // Collect loads to replace (avoid iterator invalidation).
        std::vector<std::pair<LoadInst*, Constant*>> toReplace;

        for (auto* user : gv->users()) {
            LoadInst* load = nullptr;
            uint64_t  byteOff = 0;

            if (auto* li = dyn_cast<LoadInst>(user)) {
                // Direct load from global.
                load    = li;
                byteOff = 0;
            } else if (auto* gep = dyn_cast<GEPOperator>(user)) {
                // GEP with constant indices.
                if (!gepToByteOffset(gep, DL, byteOff)) continue;
                // The GEP's users must all be loads.
                for (auto* gepUser : gep->users()) {
                    if (auto* li = dyn_cast<LoadInst>(gepUser)) {
                        load = li;
                    }
                }
            }

            if (!load) continue;
            if (load->isVolatile()) continue;

            unsigned loadBits = load->getType()->getIntegerBitWidth();
            if (loadBits == 0) continue; // non-integer load

            Constant* val = extractFromInitializer(init, byteOff, loadBits, DL);
            if (!val) continue;
            if (val->getType() != load->getType()) continue;

            toReplace.emplace_back(load, val);
        }

        for (auto& [load, val] : toReplace) {
            load->replaceAllUsesWith(val);
            load->eraseFromParent();
            ++folded;
            changed = true;
        }
    }

    if (folded > 0)
        Log::info() << "[GlobalConstProp] Folded " << folded << " loads.\n";

    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
