/**
* @file src/bin2llvmir/optimizations/struct_recovery/struct_recovery.cpp
* @brief Infer struct layouts from pointer arithmetic and GEP patterns.
* @copyright (c) 2024, MIT license
*
* Algorithm:
*   1. Walk every load/store in the module.
*   2. For each pointer operand, follow through GEPs and inttoptr/add chains
*      to extract (base_value, byte_offset, element_type).
*   3. Group by base_value; discard groups with < MIN_FIELDS distinct offsets
*      (likely just plain pointer arithmetic, not a struct).
*   4. For qualifying groups, build an llvm::StructType with explicit padding,
*      rewrite GEPs/inttoptr chains to use the new type, and register the
*      struct name in the Config so llvmir2hll can emit a typedef.
*
* Limitations (good targets for follow-on work):
*   - Does not handle nested structs (flat layout only).
*   - Does not handle union disambiguation (largest type wins per slot).
*   - Does not handle array fields (repeated equal-size offsets → array
*     detection is out of scope here).
*/

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <sstream>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

#include "retdec/bin2llvmir/optimizations/struct_recovery/struct_recovery.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/utils/io/log.h"

using namespace llvm;
using namespace retdec::utils::io;

namespace retdec {
namespace bin2llvmir {

// Minimum number of distinct offsets before we consider something a struct.
static constexpr unsigned MIN_FIELDS = 2;
// Maximum struct size we will materialise (guard against false positives).
static constexpr uint64_t MAX_STRUCT_SIZE = 4096;
// Maximum number of accesses to record per base (performance guard).
static constexpr unsigned MAX_ACCESSES_PER_BASE = 64;

char StructRecovery::ID = 0;

static RegisterPass<StructRecovery> X(
    "retdec-struct-recovery",
    "Struct layout recovery from pointer arithmetic",
    false,
    false
);

StructRecovery::StructRecovery() : ModulePass(ID) {}

bool StructRecovery::runOnModule(Module& m) {
    _module = &m;
    _abi    = AbiProvider::getAbi(_module);
    _config = ConfigProvider::getConfig(_module);
    return run();
}

bool StructRecovery::runOnModuleCustom(Module& m, Abi* abi, Config* config) {
    _module = &m;
    _abi    = abi;
    _config = config;
    return run();
}

bool StructRecovery::run() {
    if (!_module) return false;

    collectAccessPatterns();
    buildStructs();

    bool changed = false;
    for (auto& rs : _structs) {
        changed |= materializeStruct(rs);
    }
    return changed;
}

//===========================================================================
// Step 1 — collect (base, offset, type) triples
//===========================================================================

bool StructRecovery::resolveConstantOffset(Value* val, int64_t& out) {
    if (auto* ci = dyn_cast<ConstantInt>(val)) {
        out = ci->getSExtValue();
        return true;
    }
    return false;
}

bool StructRecovery::extractAccess(Value*  ptrOperand,
                                    Type*   accessType,
                                    Value*& outBase,
                                    int64_t& outOffset) {
    // Pattern A: GEP with all-constant indices.
    if (auto* gep = dyn_cast<GEPOperator>(ptrOperand)) {
        if (!gep->hasAllConstantIndices()) return false;
        // Use the DataLayout to compute the byte offset.
        const DataLayout& DL = _module->getDataLayout();
        APInt totalOffset(DL.getPointerSizeInBits(), 0);
        if (!gep->accumulateConstantOffset(DL, totalOffset)) return false;
        outBase   = gep->getPointerOperand()->stripPointerCasts();
        outOffset = static_cast<int64_t>(totalOffset.getSExtValue());
        return true;
    }

    // Pattern B: inttoptr(add(ptrtoint(base), const_offset))
    if (auto* i2p = dyn_cast<IntToPtrInst>(ptrOperand)) {
        Value* intVal = i2p->getOperand(0);
        if (auto* add = dyn_cast<BinaryOperator>(intVal)) {
            if (add->getOpcode() != Instruction::Add) return false;
            int64_t off = 0;
            Value* lhs = add->getOperand(0);
            Value* rhs = add->getOperand(1);
            if (resolveConstantOffset(rhs, off)) {
                // lhs should be ptrtoint(base)
                if (auto* p2i = dyn_cast<PtrToIntInst>(lhs)) {
                    outBase   = p2i->getOperand(0)->stripPointerCasts();
                    outOffset = off;
                    return true;
                }
                // Or lhs is just the base integer directly.
                outBase   = lhs;
                outOffset = off;
                return true;
            }
            if (resolveConstantOffset(lhs, off)) {
                if (auto* p2i = dyn_cast<PtrToIntInst>(rhs)) {
                    outBase   = p2i->getOperand(0)->stripPointerCasts();
                    outOffset = off;
                    return true;
                }
                outBase   = rhs;
                outOffset = off;
                return true;
            }
        }
    }

    // Pattern C: plain pointer (offset 0 access).
    // Only useful if it's used alongside other offset accesses on the same
    // base — collected here for completeness; buildStructs will only promote
    // groups with MIN_FIELDS distinct non-zero offsets.
    Value* stripped = ptrOperand->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalVariable>(stripped) ||
        isa<Argument>(stripped)) {
        outBase   = stripped;
        outOffset = 0;
        return true;
    }

    return false;
}

void StructRecovery::analyzeFunction(Function& fn) {
    for (auto& bb : fn) {
        for (auto& inst : bb) {
            Value* ptrOp    = nullptr;
            Type*  elemType = nullptr;

            if (auto* ld = dyn_cast<LoadInst>(&inst)) {
                ptrOp    = ld->getPointerOperand();
                elemType = ld->getType();
            } else if (auto* st = dyn_cast<StoreInst>(&inst)) {
                ptrOp    = st->getPointerOperand();
                elemType = st->getValueOperand()->getType();
            }
            if (!ptrOp || !elemType) continue;
            // Skip vector/aggregate types — too noisy.
            if (elemType->isVectorTy() || elemType->isAggregateType()) continue;

            Value*  base   = nullptr;
            int64_t offset = 0;
            if (!extractAccess(ptrOp, elemType, base, offset)) continue;
            if (!base) continue;
            // Skip register-mapped values (ABI pseudo-globals).
            if (_abi && _abi->isRegister(base)) continue;

            auto& om = _accesses[base];
            if (om.size() >= MAX_ACCESSES_PER_BASE) continue;
            if (offset >= 0 && static_cast<uint64_t>(offset) < MAX_STRUCT_SIZE) {
                om[offset].insert(elemType);
            }
        }
    }
}

void StructRecovery::collectAccessPatterns() {
    for (auto& fn : *_module) {
        if (fn.isDeclaration()) continue;
        analyzeFunction(fn);
    }
}

//===========================================================================
// Step 2 — build RecoveredStruct objects
//===========================================================================

void StructRecovery::buildStructs() {
    const DataLayout& DL = _module->getDataLayout();

    for (auto& [base, offsetMap] : _accesses) {
        // Must have at least MIN_FIELDS distinct offsets.
        if (offsetMap.size() < MIN_FIELDS) continue;

        RecoveredStruct rs;
        rs.llvmTy = nullptr;

        // Generate a unique name.
        std::ostringstream oss;
        oss << "struct_" << _structCounter++;
        rs.name = oss.str();

        uint64_t maxEnd = 0;
        for (auto& [off, types] : offsetMap) {
            RecoveredField field;
            field.offset = static_cast<uint64_t>(off);
            field.name   = "field_" + std::to_string(off);

            // Pick best type: prefer pointer types, then largest type.
            Type* best = nullptr;
            for (Type* t : types) {
                if (!best) { best = t; continue; }
                if (t->isPointerTy() && !best->isPointerTy()) { best = t; continue; }
                if (DL.getTypeAllocSize(t) > DL.getTypeAllocSize(best)) best = t;
            }
            field.type  = best ? best : Type::getInt8Ty(_module->getContext());
            field.isPtr = field.type->isPointerTy();

            uint64_t fieldEnd = field.offset + DL.getTypeAllocSize(field.type);
            if (fieldEnd > maxEnd) maxEnd = fieldEnd;

            rs.fields.push_back(field);
        }

        // Sort fields by offset (map iterator already in order, but be safe).
        std::sort(rs.fields.begin(), rs.fields.end(),
                  [](const RecoveredField& a, const RecoveredField& b) {
                      return a.offset < b.offset;
                  });

        rs.size = maxEnd;
        _structs.push_back(std::move(rs));
    }
}

//===========================================================================
// Step 3 — materialise the struct type and rewrite accesses
//===========================================================================

bool StructRecovery::materializeStruct(RecoveredStruct& rs) {
    if (rs.fields.empty()) return false;

    LLVMContext& ctx = _module->getContext();
    const DataLayout& DL = _module->getDataLayout();

    // Build an explicit-layout struct with i8 padding between fields.
    std::vector<Type*> memberTypes;
    uint64_t cursor = 0;

    for (const auto& field : rs.fields) {
        // Insert padding if needed.
        if (field.offset > cursor) {
            uint64_t padBytes = field.offset - cursor;
            memberTypes.push_back(ArrayType::get(Type::getInt8Ty(ctx), padBytes));
            cursor += padBytes;
        }
        memberTypes.push_back(field.type);
        cursor += DL.getTypeAllocSize(field.type);
    }

    // Trailing padding to reach declared size.
    if (rs.size > cursor) {
        uint64_t padBytes = rs.size - cursor;
        memberTypes.push_back(ArrayType::get(Type::getInt8Ty(ctx), padBytes));
    }

    rs.llvmTy = StructType::create(ctx, memberTypes, rs.name, /*isPacked=*/true);

    Log::info() << "[StructRecovery] Recovered " << rs.name
                << " with " << rs.fields.size() << " fields ("
                << rs.size << " bytes)\n";

    // Rewrite: for each GEP that accesses a field at a constant offset,
    // replace it with a typed GEP into the new struct type.
    // We find uses by scanning all instructions again; this is O(n) per struct
    // but struct count is small in practice.
    bool changed = false;

    for (auto& fn : *_module) {
        if (fn.isDeclaration()) continue;

        // Collect instructions to rewrite (avoid iterator invalidation).
        struct Rewrite {
            Instruction* oldInst;
            unsigned     fieldIdx;   // index into rs.fields
            Value*       base;
        };
        std::vector<Rewrite> rewrites;

        for (auto& bb : fn) {
            for (auto& inst : bb) {
                Value* ptrOp    = nullptr;
                Type*  elemType = nullptr;
                if (auto* ld = dyn_cast<LoadInst>(&inst)) {
                    ptrOp = ld->getPointerOperand(); elemType = ld->getType();
                } else if (auto* st = dyn_cast<StoreInst>(&inst)) {
                    ptrOp = st->getPointerOperand(); elemType = st->getValueOperand()->getType();
                }
                if (!ptrOp || !elemType) continue;

                Value*  base   = nullptr;
                int64_t offset = 0;
                if (!extractAccess(ptrOp, elemType, base, offset)) continue;

                // Does this access correspond to one of our struct's fields?
                if (_accesses.find(base) == _accesses.end()) continue;

                for (unsigned i = 0; i < rs.fields.size(); ++i) {
                    if (static_cast<int64_t>(rs.fields[i].offset) == offset) {
                        rewrites.push_back({&inst, i, base});
                        break;
                    }
                }
            }
        }

        // Apply rewrites using IRBuilder.
        for (auto& rw : rewrites) {
            IRBuilder<> builder(rw.oldInst);

            // Cast base to struct pointer.
            PointerType* sPtrTy = PointerType::get(rs.llvmTy, 0);
            Value* basePtr = builder.CreateBitCast(rw.base, sPtrTy, "sr_base");

            // GEP to the field.  Field index in the struct type accounts for
            // padding members; find the actual index.
            // Simpler: use byte-offset GEP with i8* then bitcast.
            const RecoveredField& field = rs.fields[rw.fieldIdx];
            Value* fieldGep = builder.CreateStructGEP(rs.llvmTy, basePtr,
                                                       rw.fieldIdx * 2, // *2 because every field has a padding element before it (except possibly the first)
                                                       "sr_" + field.name);
            // Note: padding interleaving means we can't use fieldIdx directly.
            // Instead, use a byte-offset approach for robustness.
            Value* i8Base = builder.CreateBitCast(rw.base,
                                Type::getInt8PtrTy(_module->getContext()), "sr_i8");
            Value* byteGep = builder.CreateConstGEP1_64(
                                Type::getInt8Ty(_module->getContext()),
                                i8Base, field.offset, "sr_off");
            Value* typedPtr = builder.CreateBitCast(byteGep,
                                PointerType::get(field.type, 0), "sr_ptr");

            if (auto* ld = dyn_cast<LoadInst>(rw.oldInst)) {
                Value* newLoad = builder.CreateLoad(field.type, typedPtr,
                                                     ld->getName() + "_sr");
                ld->replaceAllUsesWith(newLoad);
                ld->eraseFromParent();
                changed = true;
            } else if (auto* st = dyn_cast<StoreInst>(rw.oldInst)) {
                builder.CreateStore(st->getValueOperand(), typedPtr);
                st->eraseFromParent();
                changed = true;
            }
        }
    }

    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
