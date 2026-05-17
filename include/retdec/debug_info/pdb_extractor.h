/**
 * @file include/retdec/debug_info/pdb_extractor.h
 * @brief PDB debug-information extractor using LLVM PDB utilities.
 *
 * PdbExtractor reads a Microsoft PDB (Program Database) file and populates a
 * DebugGroundTruth.  It uses the LLVM PDB reader API (llvm/DebugInfo/PDB).
 *
 * Streams / records consumed:
 *   TPI stream  — type records: LF_STRUCTURE, LF_CLASS, LF_UNION, LF_ENUM,
 *                 LF_POINTER, LF_ARRAY, LF_PROCEDURE, LF_MFUNCTION,
 *                 LF_FIELDLIST (members, enumerators), LF_BITFIELD, LF_TYPEDEF
 *   DBI stream  — module info, section contributions, source file list
 *   GSI stream  — public symbols (S_PUB32)
 *   PSI stream  — public symbols detail
 *   Symbol records — S_LPROC32, S_GPROC32 (functions)
 *                    S_LDATA32, S_GDATA32 (globals/statics)
 *                    S_REGREL32, S_REGISTER (locals)
 *                    S_INLINESITE, S_INLINESITE_END (inlined functions)
 *   $xdata/.pdata  — MSVC SEH/EH FuncInfo + UnwindInfo (best-effort)
 *
 * The PDB extractor only works when RetDec is built with LLVM support
 * (RETDEC_USE_LLVM_PDB defined at compile time).  Otherwise it is a no-op
 * stub that immediately returns false.
 */

#ifndef RETDEC_DEBUG_INFO_PDB_EXTRACTOR_H
#define RETDEC_DEBUG_INFO_PDB_EXTRACTOR_H

#include <memory>
#include "retdec/debug_info/debug_info.h"
#include <string>

namespace retdec {
namespace debug_info {

/**
 * @brief Extracts PDB ground-truth from a .pdb file.
 *
 * Usage:
 * @code
 *   DebugGroundTruth gdt;
 *   PdbExtractor ex("/path/to/binary.pdb");
 *   ex.extract(gdt);
 * @endcode
 */
class PdbExtractor : public DebugExtractorBase {
public:
    /**
     * @param pdbPath    Full path to the .pdb file.
     * @param imageBase  Preferred base address of the matching PE image
     *                   (used to convert section-relative addresses to VAs).
     */
    explicit PdbExtractor(std::string pdbPath, uint64_t imageBase = 0);
    ~PdbExtractor() override;

    bool        extract(DebugGroundTruth& out) override;
    std::string name()    const override { return "PdbExtractor"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string pdbPath_;
    uint64_t    imageBase_ = 0;

    // ── Internal passes ───────────────────────────────────────────────────────

    /// Parse TPI stream → out.types.
    void extractTypes(DebugGroundTruth& out);

    /// Parse DBI + GSI/PSI → global/public symbol names.
    void extractSymbols(DebugGroundTruth& out);

    /// Parse per-module symbol records → DebugFunc entries.
    void extractFunctions(DebugGroundTruth& out);

    /// Parse $xdata/.pdata for MSVC EH metadata (non-returning, EH sites).
    void extractEHData(DebugGroundTruth& out);
};

} // namespace debug_info
} // namespace retdec

#endif // RETDEC_DEBUG_INFO_PDB_EXTRACTOR_H
