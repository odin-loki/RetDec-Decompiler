/**
 * @file include/retdec/rtti/rtti.h
 * @brief ABI-agnostic RTTI/vtable data model and reconstructor interface.
 *
 * ## Overview
 *
 * Two ABI-specific parsers (Itanium, MSVC) both produce a ClassHierarchyGraph.
 * The graph nodes are ClassNode objects; edges are InheritanceEdge objects
 * encoding the base-class relationship, byte offset, and virtualness.
 *
 * VtableEntry ties a vtable slot to a known function entry point (VMA).
 * VtableInfo describes one contiguous vtable (possibly one of many for a
 * class with multiple or virtual bases).
 *
 * ## Itanium ABI vtable layout
 *
 *   vtable[-2]  offset-to-top  (ptrdiff_t)
 *   vtable[-1]  type_info ptr  (pointer to std::type_info subclass)
 *   vtable[0..] virtual function pointers
 *
 * A class with multiple or virtual bases has multiple sub-vtables separated
 * by additional [offset-to-top][type_info] header pairs.
 *
 * ## MSVC ABI vtable layout
 *
 *   vtable[-1]  _RTTICompleteObjectLocator*  (COL)
 *   vtable[0..] virtual function pointers
 *
 * COL → _RTTITypeDescriptor (mangled class name)
 *     → _RTTIClassHierarchyDescriptor
 *         → _RTTIBaseClassDescriptor[] (per base: PMD offsets, mangled name)
 */

#ifndef RETDEC_RTTI_RTTI_H
#define RETDEC_RTTI_RTTI_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace rtti {

// ─── Vtable entry / vtable record ─────────────────────────────────────────────

struct VtableEntry {
    uint64_t slotVma;     ///< VMA of this slot in the binary
    uint64_t targetVma;   ///< VMA the slot points to (function entry)
    bool     isPureVirtual = false;
    bool     isNull        = false;
};

struct VtableInfo {
    uint64_t                   vtableVma;    ///< VMA of first slot (past header)
    uint64_t                   typeInfoVma;  ///< VMA of the type_info ptr (Itanium)
    int64_t                    offsetToTop;  ///< offset-to-top value
    std::vector<VtableEntry>   slots;
    uint32_t                   subVtableIdx; ///< 0 = primary, >0 = secondary
};

// ─── Class hierarchy graph ─────────────────────────────────────────────────────

enum class InheritanceKind : uint8_t {
    Direct,
    Virtual,
};

struct InheritanceEdge {
    std::string      baseName;      ///< Demangled base class name
    int32_t          byteOffset;    ///< Byte offset of base within derived
    int32_t          vbaseOffset;   ///< vbase table offset (virtual inheritance)
    int32_t          vdispOffset;   ///< vdisp index (MSVC virtual)
    InheritanceKind  kind           = InheritanceKind::Direct;
    uint32_t         numContained   = 0; ///< MSVC: numContainedBases
};

struct ClassNode {
    std::string                  name;          ///< Demangled class name
    std::string                  mangledName;
    uint64_t                     typeInfoVma  = 0; ///< Itanium: type_info VMA
    uint64_t                     colVma       = 0; ///< MSVC: COL VMA
    std::vector<VtableInfo>      vtables;          ///< All vtables for this class
    std::vector<InheritanceEdge> bases;            ///< Direct base edges
    bool                         isAbstract   = false;
    bool                         isPolymorphic= false;
};

struct ClassHierarchyGraph {
    /// All class nodes, keyed by demangled name.
    std::unordered_map<std::string, ClassNode>  classes;

    /// Diagnostics from the parser.
    std::vector<std::string>                    diagnostics;

    // ── Lookup helpers ────────────────────────────────────────────────────────

    const ClassNode* byName(const std::string& name) const noexcept;
    const ClassNode* byTypeInfoVma(uint64_t vma)      const noexcept;
    const ClassNode* byColVma(uint64_t vma)            const noexcept;

    /// Return all classes that directly inherit from `baseName`.
    std::vector<const ClassNode*> directDerivedFrom(
        const std::string& baseName) const;

    bool empty() const noexcept { return classes.empty(); }
};

// ─── Binary image view (read-only, provided by the caller) ────────────────────

struct BinarySection {
    uint64_t    vma;
    uint64_t    size;
    bool        executable;
    bool        writable;
    bool        readable;
    const uint8_t* data; ///< Host pointer to section bytes
};

/**
 * Minimal read-only view of the loaded binary image.
 *
 * Callers fill this structure from LoadedImage or directly from a PE/ELF
 * parser.  Both Itanium and MSVC parsers consume this interface.
 */
struct BinaryView {
    std::vector<BinarySection> sections;
    uint64_t                   imageBase     = 0;
    bool                       is64bit       = true;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /// Read a pointer-sized value at `vma`; returns 0 on out-of-bounds.
    uint64_t readPtr(uint64_t vma) const noexcept;

    /// Read a 4-byte value at `vma`; returns 0 on out-of-bounds.
    uint32_t read32(uint64_t vma) const noexcept;

    /// Read a signed 4-byte value.
    int32_t  readI32(uint64_t vma) const noexcept;

    /// Read a pointer-sized signed value.
    int64_t  readIPtr(uint64_t vma) const noexcept;

    /// Read a null-terminated string at `vma` (up to maxLen bytes).
    std::string readCStr(uint64_t vma, std::size_t maxLen = 256) const;

    /// Return the section containing `vma`, or nullptr.
    const BinarySection* sectionAt(uint64_t vma) const noexcept;

    bool inExecutable(uint64_t vma) const noexcept;
    bool inData(uint64_t vma)       const noexcept;
    bool inReadOnly(uint64_t vma)   const noexcept;

    uint32_t ptrSize() const noexcept { return is64bit ? 8 : 4; }
};

// ─── Reconstructor interface ──────────────────────────────────────────────────

/**
 * Abstract base for ABI-specific RTTI/vtable reconstructors.
 */
class VtableReconstructorBase {
public:
    virtual ~VtableReconstructorBase() = default;

    /**
     * Scan the binary view for vtables and RTTI structures.
     *
     * @param view      Read-only binary image.
     * @param funcVmas  Set of known function entry points (from FuncBoundaryDetector).
     *                  Used to validate vtable slot targets.
     * @param out       Output graph.
     * @return true on success (even partial).
     */
    virtual bool reconstruct(
        const BinaryView&                view,
        const std::vector<uint64_t>&     funcVmas,
        ClassHierarchyGraph&             out) = 0;

    virtual std::string name() const = 0;
};

} // namespace rtti
} // namespace retdec

#endif // RETDEC_RTTI_RTTI_H
