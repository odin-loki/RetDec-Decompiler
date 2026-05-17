/**
 * @file include/retdec/func_boundary/func_boundary.h
 * @brief Three-pass convergent function boundary detection.
 *
 * ## Algorithm
 *
 * ### Pass 1 — Direct Evidence (confidence 0.95–1.0)
 *   Sources fed to the detector:
 *   - Entry point of the binary
 *   - All direct CALL targets found during linear scan
 *   - Exports / public symbols
 *   - TLS callbacks
 *   - SEH / DWARF exception-handler addresses
 *
 * ### Pass 2 — Prologue Scan (confidence 0.60–0.85)
 *   Compiler-specific byte patterns searched in executable sections:
 *   - GCC x86-64  : {55 48 89 E5}  (push rbp; mov rbp,rsp) or push-register
 *                   sequences
 *   - MSVC x86-64 : shadow-space setup pattern (sub rsp, N where N % 16 == 8)
 *   - Clang       : similar to GCC with REX.W prefix variants
 *   Score partial matches; only candidates with score ≥ 0.7 are promoted.
 *
 * ### Pass 3 — Non-Returning Propagation
 *   Seeds: exit, _exit, abort, __stack_chk_fail, longjmp, __longjmp_chk,
 *          _Noreturn-annotated (from import names).
 *   Fixpoint: if every exit path of function F calls a non-returner, F is
 *   also marked non-returning.  Callers of non-returners have the basic block
 *   terminated after the call with no fallthrough edge.
 *
 * ### Thunk detection
 *   A function consisting of a single JMP to an import stub or another
 *   function is classified as a thunk and renamed thunk_<target_name>.
 *
 * ## Usage
 *
 *   FuncBoundaryDetector det(arch, imageBase, data, size);
 *   det.addEntryPoint(0x401000);
 *   det.addCallTarget(0x401020);
 *   det.addSymbol("my_func", 0x401040, SymbolSource::Export);
 *   det.addImport(0x6000, "kernel32.dll", "ExitProcess");
 *   det.runPass1();
 *   det.runPass2(compilerProfile);
 *   det.runPass3();
 *   for (auto& fb : det.functions()) { ... }
 */

#ifndef RETDEC_FUNC_BOUNDARY_FUNC_BOUNDARY_H
#define RETDEC_FUNC_BOUNDARY_FUNC_BOUNDARY_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace func_boundary {

// ─── Evidence source ─────────────────────────────────────────────────────────

enum class EvidenceSource : uint8_t {
    EntryPoint,      ///< Binary entry point
    CallTarget,      ///< Direct CALL instruction target
    Export,          ///< Exported / public symbol
    DebugSymbol,     ///< Debug-info symbol (DWARF/PDB)
    TLSCallback,     ///< TLS callback pointer
    ExceptionHandler,///< SEH or DWARF EH handler address
    PrologueFull,    ///< Complete compiler prologue pattern match
    ProloguePartial, ///< Partial prologue pattern match
    Heuristic,       ///< Generic heuristic (alignment, etc.)
};

/// Confidence weight per evidence source.
inline double evidenceConfidence(EvidenceSource src) noexcept
{
    switch (src) {
    case EvidenceSource::EntryPoint:       return 1.00;
    case EvidenceSource::CallTarget:       return 0.98;
    case EvidenceSource::Export:           return 0.95;
    case EvidenceSource::DebugSymbol:      return 0.99;
    case EvidenceSource::TLSCallback:      return 0.95;
    case EvidenceSource::ExceptionHandler: return 0.90;
    case EvidenceSource::PrologueFull:     return 0.85;
    case EvidenceSource::ProloguePartial:  return 0.60;
    case EvidenceSource::Heuristic:        return 0.50;
    }
    return 0.50;
}

// ─── Compiler profile hint (for prologue selection in Pass 2) ─────────────────

enum class CompilerHint : uint8_t {
    Unknown,
    GCC,
    Clang,
    MSVC,
};

// ─── Function boundary record ─────────────────────────────────────────────────

struct FunctionBoundary {
    uint64_t      startAddr    = 0;      ///< First byte of the function
    uint64_t      endAddr      = 0;      ///< One past last byte (best estimate)
    double        confidence   = 0.0;    ///< Aggregate confidence [0,1]
    EvidenceSource primaryEvidence = EvidenceSource::Heuristic;
    std::string   name;                  ///< Symbol name (empty if unknown)
    bool          isNonReturning = false;///< Function never returns
    bool          isThunk        = false;///< Single-JMP thunk
    std::string   thunkTarget;           ///< Name of thunk target (if isThunk)

    // All evidence sources that contributed.
    std::vector<EvidenceSource> allEvidence;
};

// ─── Import record (for non-returning detection and thunk naming) ──────────────

struct ImportEntry {
    uint64_t    vma;     ///< IAT/PLT entry address
    std::string dll;
    std::string symbol;
};

// ─── Prologue pattern ─────────────────────────────────────────────────────────

struct ProloguePattern {
    std::string      name;           ///< e.g. "gcc_frame_setup"
    std::vector<int> bytes;          ///< -1 = wildcard byte
    double           fullScore;      ///< Confidence if fully matched
    double           partialScore;   ///< Confidence if partially matched (≥50%)
};

// ─── FuncBoundaryDetector ────────────────────────────────────────────────────

class FuncBoundaryDetector {
public:
    /**
     * @param imageBase  Virtual base address of the image.
     * @param data       Raw image bytes (not owned; caller keeps alive).
     * @param size       Image byte count.
     * @param is64Bit    True for 64-bit mode.
     */
    FuncBoundaryDetector(uint64_t       imageBase,
                         const uint8_t* data,
                         std::size_t    size,
                         bool           is64Bit = true);

    ~FuncBoundaryDetector() = default;

    // ── Evidence injection ────────────────────────────────────────────────────

    void addEntryPoint(uint64_t addr);
    void addCallTarget(uint64_t addr);
    void addSymbol(const std::string& name, uint64_t addr, EvidenceSource src);
    void addTLSCallback(uint64_t addr);
    void addExceptionHandler(uint64_t addr);

    /// Register an executable section so Pass 2 knows where to scan.
    void addExecutableSection(uint64_t start, uint64_t end);

    /// Register an import for non-returning detection and thunk naming.
    void addImport(uint64_t vma, const std::string& dll,
                   const std::string& symbol);

    // ── Pipeline ──────────────────────────────────────────────────────────────

    /// Pass 1: commit all direct-evidence function starts.
    void runPass1();

    /**
     * Pass 2: scan executable sections for compiler prologues.
     * @param hint  Compiler hint from the CompilerFingerprinter (Stage 4).
     */
    void runPass2(CompilerHint hint = CompilerHint::Unknown);

    /// Pass 3: non-returning propagation + thunk detection.
    void runPass3();

    /// Run all three passes in order.
    void runAll(CompilerHint hint = CompilerHint::Unknown);

    // ── Results ───────────────────────────────────────────────────────────────

    /// All detected function boundaries, sorted by startAddr.
    const std::vector<FunctionBoundary>& functions() const noexcept;

    /// Lookup by start address. Returns nullptr if not found.
    const FunctionBoundary* functionAt(uint64_t addr) const noexcept;

    /// True if the function at addr is known non-returning.
    bool isNonReturning(uint64_t addr) const noexcept;

    /// True if addr is a thunk.
    bool isThunk(uint64_t addr) const noexcept;

    // ── Prologue catalogue (exposed for testing) ──────────────────────────────

    static std::vector<ProloguePattern> prologuePatterns(CompilerHint hint);

    /// Score a byte sequence against a prologue pattern.
    /// Returns the score (0.0 if no match, partialScore for ≥50%, fullScore for full).
    static double matchPrologue(const ProloguePattern& pat,
                                const uint8_t*         bytes,
                                std::size_t            available);

    // ── Non-returning seed set (exposed for testing) ──────────────────────────

    static bool isKnownNonReturner(const std::string& symbolName) noexcept;

private:
    uint64_t       _imageBase;
    const uint8_t* _data;
    std::size_t    _size;
    bool           _is64Bit;

    // Candidate table: addr → best record so far.
    std::unordered_map<uint64_t, FunctionBoundary> _candidates;

    // Executable sections.
    struct ExecSection { uint64_t start, end; };
    std::vector<ExecSection> _execSections;

    // Imports map: vma → ImportEntry.
    std::unordered_map<uint64_t, ImportEntry> _imports;
    // Name → VMA map.
    std::unordered_map<std::string, uint64_t> _importByName;

    // Non-returning set (function start VMAs).
    std::unordered_set<uint64_t> _nonReturning;

    // Finalised, sorted result list.
    mutable std::vector<FunctionBoundary> _sorted;
    mutable bool                          _sortedDirty = true;

    // ── Internal helpers ──────────────────────────────────────────────────────

    void ensureCandidate(uint64_t addr, EvidenceSource src,
                         const std::string& name = std::string{});
    void updateConfidence(FunctionBoundary& fb, EvidenceSource src);

    std::size_t vaToOffset(uint64_t va) const noexcept;

    uint8_t  readU8(std::size_t off)  const noexcept;
    uint32_t readU32(std::size_t off) const noexcept;
    uint64_t readU64(std::size_t off) const noexcept;

    // Pass 2 internal: scan one section.
    void scanSectionPrologues(uint64_t secStart, uint64_t secEnd,
                              const std::vector<ProloguePattern>& patterns);

    // Pass 2: detect CALL targets by linear scan of the section.
    void scanCallTargets(uint64_t secStart, uint64_t secEnd);

    // Pass 3: non-returning seed + fixpoint.
    void seedNonReturning();
    void propagateNonReturning();

    // Pass 3: thunk detection.
    void detectThunks();

    // Check if the bytes at `off` look like a single-JMP thunk.
    // Returns the target VMA, or 0 if not a thunk.
    uint64_t detectThunkAt(std::size_t off) const noexcept;

    std::string nameForVma(uint64_t vma) const;
};

} // namespace func_boundary
} // namespace retdec

#endif // RETDEC_FUNC_BOUNDARY_FUNC_BOUNDARY_H
