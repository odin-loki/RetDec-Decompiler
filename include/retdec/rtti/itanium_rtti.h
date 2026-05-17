/**
 * @file include/retdec/rtti/itanium_rtti.h
 * @brief Itanium C++ ABI RTTI and vtable reconstructor.
 *
 * Itanium ABI type_info hierarchy (from <cxxabi.h>):
 *
 *   __fundamental_type_info        — scalar types
 *   __array_type_info               — arrays
 *   __function_type_info            — function types
 *   __enum_type_info                — enums
 *   __class_type_info               — class with no bases
 *   __si_class_type_info            — single public non-virtual inheritance
 *       [vtable ptr, name ptr, base type_info ptr]
 *   __vmi_class_type_info           — multiple or virtual inheritance
 *       [vtable ptr, name ptr, flags, base_count, base_info[]]
 *       each base_info = { type_info*, offset_flags (ptrdiff_t) }
 *
 * Vtable structure:
 *   [offset-to-top] [type_info*] [vfptr0] [vfptr1] ...
 * Multiple sub-vtables for MI:
 *   ... [offset-to-top] [type_info*] [vfptr0] ... (next component vtable)
 *
 * Known type_info vtable symbols used for validation:
 *   _ZTVN10__cxxabiv117__class_type_infoE
 *   _ZTVN10__cxxabiv120__si_class_type_infoE
 *   _ZTVN10__cxxabiv121__vmi_class_type_infoE
 *   _ZTVN10__cxxabiv116__enum_type_infoE
 */

#ifndef RETDEC_RTTI_ITANIUM_RTTI_H
#define RETDEC_RTTI_ITANIUM_RTTI_H

#include "retdec/rtti/rtti.h"
#include <unordered_set>

namespace retdec {
namespace rtti {

class ItaniumRttiReconstructor : public VtableReconstructorBase {
public:
    ItaniumRttiReconstructor() = default;

    bool reconstruct(
        const BinaryView&            view,
        const std::vector<uint64_t>& funcVmas,
        ClassHierarchyGraph&         out) override;

    std::string name() const override { return "ItaniumRttiReconstructor"; }

    // ── Public helpers (exposed for testing) ─────────────────────────────────

    /// Return true if `vma` looks like a valid Itanium vtable start.
    bool isValidVtable(
        const BinaryView&             view,
        const std::unordered_set<uint64_t>& funcSet,
        uint64_t                      vtableVma) const;

    /// Parse one type_info object; populate class node in `out`.
    /// Returns the demangled class name, or empty on failure.
    std::string parseTypeInfo(
        const BinaryView&  view,
        uint64_t           tiVma,
        ClassHierarchyGraph& out);

private:
    // Known type_info vtable VMAs discovered during the scan.
    std::unordered_set<uint64_t> knownTiVtables_;

    // VMAs of type_info objects already processed (avoid re-entry).
    std::unordered_set<uint64_t> visitedTi_;

    // ── Internal passes ───────────────────────────────────────────────────────

    /// Pass 1: build knownTiVtables_ from named symbols / pattern scan.
    void discoverTiVtables(const BinaryView& view, ClassHierarchyGraph& out);

    /// Pass 2: scan .data/.rodata for candidate vtable headers.
    void scanVtables(
        const BinaryView&             view,
        const std::unordered_set<uint64_t>& funcSet,
        ClassHierarchyGraph&          out);

    /// Parse vtable slots starting at `firstSlotVma`.
    VtableInfo parseVtableSlots(
        const BinaryView&             view,
        const std::unordered_set<uint64_t>& funcSet,
        uint64_t                      firstSlotVma,
        uint64_t                      typeInfoVma,
        int64_t                       offsetToTop,
        uint32_t                      subIdx) const;

    /// Parse __si_class_type_info bases.
    void parseSiClassTypeInfo(
        const BinaryView&  view,
        uint64_t           tiVma,
        ClassNode&         node,
        ClassHierarchyGraph& out);

    /// Parse __vmi_class_type_info bases.
    void parseVmiClassTypeInfo(
        const BinaryView&  view,
        uint64_t           tiVma,
        ClassNode&         node,
        ClassHierarchyGraph& out);

    /// Read the null-terminated mangled name from a type_info name field.
    std::string readTiName(const BinaryView& view, uint64_t tiVma) const;

public:
    /// Demangle a mangled name using __cxa_demangle logic (pure C++ fallback).
    static std::string demangle(const std::string& mangled);
};

} // namespace rtti
} // namespace retdec

#endif // RETDEC_RTTI_ITANIUM_RTTI_H
