/**
 * @file include/retdec/debug_info/dwarf_extractor.h
 * @brief DWARF debug-information extractor using libdw (elfutils).
 *
 * DwarfExtractor walks the DWARF DIE tree of an ELF binary and populates a
 * DebugGroundTruth.  It uses libdw (from elfutils) for low-level DWARF parsing
 * and delegates DWARF location expression evaluation to DebugLocEvaluator.
 *
 * Supported DIE tags:
 *   DW_TAG_compile_unit       → DebugSourceFile
 *   DW_TAG_subprogram         → DebugFunc (name, ranges, return type, params)
 *   DW_TAG_formal_parameter   → DebugVar (isParam=true) inside DebugFunc
 *   DW_TAG_variable           → DebugVar (local) inside DebugFunc
 *   DW_TAG_inlined_subroutine → InlinedSite inside DebugFunc
 *   DW_TAG_base_type          → DebugType (Primitive)
 *   DW_TAG_pointer_type       → DebugType (Pointer)
 *   DW_TAG_array_type         → DebugType (Array)
 *   DW_TAG_structure_type     → DebugType (Struct)
 *   DW_TAG_class_type         → DebugType (Struct)
 *   DW_TAG_union_type         → DebugType (Union)
 *   DW_TAG_enumeration_type   → DebugType (Enum)
 *   DW_TAG_typedef            → DebugType (Typedef)
 *   DW_TAG_subroutine_type    → DebugType (FunctionPtr)
 *   DW_TAG_member             → DebugField inside Struct/Union
 *   DW_TAG_enumerator         → DebugEnumerator inside Enum
 */

#ifndef RETDEC_DEBUG_INFO_DWARF_EXTRACTOR_H
#define RETDEC_DEBUG_INFO_DWARF_EXTRACTOR_H

#include <memory>
#include "retdec/debug_info/debug_info.h"
#include <string>
#include <vector>

namespace retdec {
namespace debug_info {

/**
 * @brief Extracts DWARF ground-truth from an ELF file.
 *
 * Usage:
 * @code
 *   DebugGroundTruth gdt;
 *   DwarfExtractor ex("/path/to/binary");
 *   ex.extract(gdt);
 * @endcode
 */
class DwarfExtractor : public DebugExtractorBase {
public:
    explicit DwarfExtractor(std::string filePath);
    ~DwarfExtractor() override;

    bool        extract(DebugGroundTruth& out) override;
    std::string name()    const override { return "DwarfExtractor"; }

    /// Return the address size (4 or 8) found in the first CU header.
    uint8_t addrSize() const noexcept { return addrSize_; }

private:
    // ── Implementation detail (libdw-backed) ─────────────────────────────────
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string filePath_;
    uint8_t     addrSize_ = 8;

    // ── Internal passes ───────────────────────────────────────────────────────

    /// First pass: walk all CUs to collect all type DIEs into out.types.
    void collectTypes(DebugGroundTruth& out);

    /// Second pass: walk all CUs to extract subprograms + variables.
    void collectFunctions(DebugGroundTruth& out);

    /// Third pass: collect compile-unit source file records.
    void collectSourceFiles(DebugGroundTruth& out);
};

} // namespace debug_info
} // namespace retdec

#endif // RETDEC_DEBUG_INFO_DWARF_EXTRACTOR_H
