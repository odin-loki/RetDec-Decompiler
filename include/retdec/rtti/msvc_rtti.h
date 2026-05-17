/**
 * @file include/retdec/rtti/msvc_rtti.h
 * @brief MSVC ABI RTTI and vtable reconstructor.
 *
 * MSVC vtable layout (32-bit and 64-bit):
 *   vtable[-1]  pointer to _RTTICompleteObjectLocator (COL)
 *   vtable[0..] virtual function pointers
 *
 * _RTTICompleteObjectLocator (COL):
 *   32-bit: { signature, offset, cdOffset, pTypeDescriptor*, pClassDescriptor* }
 *   64-bit: { signature, offset, cdOffset, typeDescriptorOffset (RVA),
 *              classDescriptorOffset (RVA), selfRVA }
 *
 * _RTTITypeDescriptor:
 *   { vtable ptr (points to type_info vtable), spare (0), name[] (mangled) }
 *
 * _RTTIClassHierarchyDescriptor:
 *   { signature, attributes, numBaseClasses, pBaseClassArray (or RVA) }
 *
 * _RTTIBaseClassDescriptor:
 *   { pTypeDescriptor (or RVA), numContainedBases,
 *     PMD { mdisp, pdisp, vdisp }, attributes }
 *
 * PMD (Pointer-to-Member Displacement):
 *   mdisp  — member displacement (offset of base subobject)
 *   pdisp  — vbtable displacement (−1 if not virtual)
 *   vdisp  — virtual base table offset
 */

#ifndef RETDEC_RTTI_MSVC_RTTI_H
#define RETDEC_RTTI_MSVC_RTTI_H

#include "retdec/rtti/rtti.h"
#include <unordered_set>

namespace retdec {
namespace rtti {

class MsvcRttiReconstructor : public VtableReconstructorBase {
public:
    MsvcRttiReconstructor() = default;

    bool reconstruct(
        const BinaryView&            view,
        const std::vector<uint64_t>& funcVmas,
        ClassHierarchyGraph&         out) override;

    std::string name() const override { return "MsvcRttiReconstructor"; }

    // ── Public helpers (exposed for testing) ─────────────────────────────────

    /// Return true if `colVma` points to a valid COL.
    bool isValidCol(const BinaryView& view, uint64_t colVma) const;

    /// Parse a COL at `colVma`; return the demangled class name or "".
    std::string parseCol(
        const BinaryView&  view,
        uint64_t           colVma,
        ClassHierarchyGraph& out);

private:
    std::unordered_set<uint64_t> visitedCol_;

    // ── Internal passes ───────────────────────────────────────────────────────

    void scanVtables(
        const BinaryView&             view,
        const std::unordered_set<uint64_t>& funcSet,
        ClassHierarchyGraph&          out);

    VtableInfo parseVtableSlots(
        const BinaryView&             view,
        const std::unordered_set<uint64_t>& funcSet,
        uint64_t                      vtableVma) const;

    bool parseClassHierarchy(
        const BinaryView&  view,
        uint64_t           hierVma,  ///< _RTTIClassHierarchyDescriptor VMA
        uint64_t           imageBase,
        ClassNode&         node,
        ClassHierarchyGraph& out);

    /// Read a pointer-or-RVA depending on `is64bit` and return the VMA.
    uint64_t readPtrOrRva(
        const BinaryView& view,
        uint64_t          fieldVma,
        uint64_t          imageBase) const;

    /// Read a MSVC mangled class name from type descriptor at `tdVma`.
    std::string readTdName(const BinaryView& view, uint64_t tdVma) const;

public:
    /// Demangle a MSVC mangled name (strips leading '?' and trailing '@').
    static std::string demangle(const std::string& mangled);
};

} // namespace rtti
} // namespace retdec

#endif // RETDEC_RTTI_MSVC_RTTI_H
