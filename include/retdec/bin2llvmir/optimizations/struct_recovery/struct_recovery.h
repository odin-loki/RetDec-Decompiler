/**
* @file include/retdec/bin2llvmir/optimizations/struct_recovery/struct_recovery.h
* @brief Infer struct layouts from pointer arithmetic and GEP patterns.
* @copyright (c) 2024, MIT license
*
* Scans the LLVM IR for pointer arithmetic sequences of the form:
*   base_ptr + constant_offset → load/store of some type
* and groups them by base pointer to recover struct field layouts.
* Recovered structs are registered with the Config so downstream passes
* (llvmir2hll) can emit named struct types instead of raw pointer casts.
*/

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_STRUCT_RECOVERY_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_STRUCT_RECOVERY_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"

namespace retdec {
namespace bin2llvmir {

/// One inferred field inside a recovered struct.
struct RecoveredField {
    uint64_t    offset;     ///< Byte offset from struct base.
    llvm::Type* type;       ///< Inferred LLVM type of this field.
    std::string name;       ///< Generated name, e.g. "field_8".
    bool        isPtr;      ///< True if field type is a pointer.
};

/// One recovered struct layout.
struct RecoveredStruct {
    std::string                   name;    ///< e.g. "struct_0x4010a0"
    std::vector<RecoveredField>   fields;  ///< Sorted by offset.
    uint64_t                      size;    ///< Total size in bytes (max offset + field size).
    llvm::StructType*             llvmTy;  ///< Created LLVM struct type, may be nullptr.
};

class StructRecovery : public llvm::ModulePass {
public:
    static char ID;
    StructRecovery();
    virtual bool runOnModule(llvm::Module& m) override;
    bool runOnModuleCustom(llvm::Module& m, Abi* abi, Config* config);

    /// Read-only access to recovered structs after the pass runs.
    const std::vector<RecoveredStruct>& getRecoveredStructs() const {
        return _structs;
    }

private:
    bool run();

    /// Collect (base_value → offset → access_type) from all GEPs and
    /// inttoptr+add sequences across the whole module.
    void collectAccessPatterns();

    /// From the raw access map, build RecoveredStruct objects.
    void buildStructs();

    /// Create an llvm::StructType for @a rs and rewrite all accesses to use it.
    bool materializeStruct(RecoveredStruct& rs);

    /// Walk a function looking for ptr+offset patterns.
    void analyzeFunction(llvm::Function& fn);

    /// Try to extract (base, constant_offset, access_type) from a load/store
    /// pointer operand. Returns false if pattern not recognised.
    bool extractAccess(llvm::Value* ptrOperand,
                       llvm::Type* accessType,
                       llvm::Value*& outBase,
                       int64_t&      outOffset);

    /// Resolve an integer constant or a constant GEP byte offset.
    bool resolveConstantOffset(llvm::Value* val, int64_t& out);

private:
    llvm::Module* _module = nullptr;
    Abi*          _abi    = nullptr;
    Config*       _config = nullptr;

    // Map: base_value → (offset → set of LLVM types seen at that offset).
    using TypeSet = std::set<llvm::Type*>;
    using OffsetMap = std::map<int64_t, TypeSet>;
    std::map<llvm::Value*, OffsetMap> _accesses;

    std::vector<RecoveredStruct> _structs;
    unsigned _structCounter = 0;
};

} // namespace bin2llvmir
} // namespace retdec

#endif // RETDEC_BIN2LLVMIR_OPTIMIZATIONS_STRUCT_RECOVERY_H
