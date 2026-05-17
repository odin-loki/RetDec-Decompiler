/**
 * @file include/retdec/rtti/symbian_rtti.h
 * @brief Symbian OS / EPOC C++ RTTI reconstructor.
 *
 * ## Symbian class conventions
 *
 * Symbian does not use standard C++ RTTI.  Instead it has a framework-level
 * type system based on naming conventions and the CBase root class:
 *
 *   T-classes — simple value types (no vtable, no cleanup)
 *   C-classes — heap objects derived from CBase, have vtables
 *   R-classes — resource handles (no vtable)
 *   M-classes — mixin interfaces (pure-virtual vtables, no CBase)
 *   E-classes — enumerations (no vtable)
 *
 * ## CBase vtable layout (ARM32 Symbian OS)
 *
 *   vtable[0]  = (reserved, always 0 in CBase itself)
 *   vtable[1]  = ~CBase() destructor
 *
 * For derived classes the first two slots are overridden, followed by user
 * virtual methods.  There is no negative-offset RTTI header.
 *
 * ## TMetaClass structure (EPOC32 runtime, `e32base.h`)
 *
 * Some Symbian versions embed a TMetaClass at vtable[-1] to support
 * runtime class checking via `IsA()`:
 *
 *   struct TMetaClass {
 *       uint32_t    iSize;     // sizeof(class)
 *       uint32_t    iOffset;   // offset of CBase subobject (usually 0)
 *       const char* iName;     // ASCII class name (null-terminated)
 *       TMetaClass* iParent;   // parent's TMetaClass (or nullptr)
 *   };
 *
 * ## Scan strategy
 *
 * 1. Scan for candidate TMetaClass blocks in data sections:
 *    - [small_uint32 size][0 or small_offset][ptr_to_ascii_string][ptr_to_data_or_0]
 * 2. The ASCII name must start with 'C', 'M', or 'R' (Symbian conventions).
 * 3. The vtable that references this TMetaClass at slot[-1] must have
 *    ARM function pointers in slot[0..] (odd addresses due to Thumb interwork).
 * 4. Follow iParent chains to reconstruct the class hierarchy.
 *
 * ## Fallback: name-pattern scan
 *
 * If no TMetaClass blocks are found, scan read-only data for null-terminated
 * strings matching the Symbian naming convention (`^[CTMR][A-Z][a-zA-Z0-9]+$`)
 * and associate them with nearby vtables.
 */

#ifndef RETDEC_RTTI_SYMBIAN_RTTI_H
#define RETDEC_RTTI_SYMBIAN_RTTI_H

#include "retdec/rtti/rtti.h"
#include <regex>
#include <unordered_set>

namespace retdec {
namespace rtti {

struct TMetaClassInfo {
    uint64_t    vma        = 0;
    uint32_t    iSize      = 0;
    uint32_t    iOffset    = 0;
    std::string iName;
    uint64_t    iParentVma = 0;
};

class SymbianRttiReconstructor : public VtableReconstructorBase {
public:
    SymbianRttiReconstructor() = default;

    bool reconstruct(
        const BinaryView&            view,
        const std::vector<uint64_t>& funcVmas,
        ClassHierarchyGraph&         out) override;

    std::string name() const override { return "SymbianRttiReconstructor"; }

    bool isValidTMetaClass(const BinaryView& view, uint64_t vma) const;
    bool parseTMetaClass(const BinaryView& view, uint64_t vma,
                          TMetaClassInfo& info) const;

    /// Return true if `name` matches Symbian class naming conventions.
    static bool isSymbianClassName(const std::string& name);

private:
    std::unordered_set<uint64_t> visited_;

    void scanTMetaClass(const BinaryView& view,
                        ClassHierarchyGraph& out);

    void scanNamePattern(const BinaryView& view,
                         const std::unordered_set<uint64_t>& funcSet,
                         ClassHierarchyGraph& out);
};

} // namespace rtti
} // namespace retdec

#endif // RETDEC_RTTI_SYMBIAN_RTTI_H
