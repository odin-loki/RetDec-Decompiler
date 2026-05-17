/**
 * @file include/retdec/code_data/code_data_classifier.h
 * @brief Multi-evidence Bayesian classifier for code vs. data discrimination.
 *
 * ## Architecture
 *
 * The classifier maintains a per-byte probability table over the executable
 * sections of a binary.  Five independent evidence sources update a running
 * log-odds score for each byte address:
 *
 *   1. ReachabilityEvidence  — reachable from entry point(s) via CFG
 *   2. InstructionValidity   — does a valid instruction decode here?
 *   3. AlignmentEvidence     — function/branch-target alignment (GCC/MSVC)
 *   4. ReferenceTypeEvidence — referenced via CALL/JMP (code) or LEA/MOV-imm (data)
 *   5. ARMThumbEvidence      — LSB of pointer in ARM binaries
 *
 * Bayesian update in log-odds space:
 *   logOdds += log(P(evidence | code) / P(evidence | data))
 *
 * Final classification:
 *   posterior > 0.7  → CODE
 *   posterior < 0.3  → DATA
 *   otherwise        → AMBIGUOUS
 *
 * ## Usage
 *
 *   CodeDataClassifier clf(arch, imageBase, data, size);
 *   clf.addEntryPoint(0x401000);
 *   clf.addReachable(0x401000, 0x401100);
 *   clf.addValidInstruction(0x401005, 3);
 *   clf.addReference(0x401020, RefType::Call);
 *   clf.addARMThumbPointer(0x401040 | 1);
 *   clf.classify();
 *
 *   for (auto& r : clf.results()) {
 *       if (r.label == Label::Code) { ... }
 *   }
 */

#ifndef RETDEC_CODE_DATA_CODE_DATA_CLASSIFIER_H
#define RETDEC_CODE_DATA_CODE_DATA_CLASSIFIER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace code_data {

// ─── Architecture tag ─────────────────────────────────────────────────────────

enum class Arch {
    X86,    ///< x86 32-bit
    X86_64, ///< x86-64
    ARM,    ///< ARM 32-bit (AArch32, includes Thumb)
    ARM64,  ///< AArch64
    MIPS,   ///< MIPS 32-bit
    MIPS64, ///< MIPS 64-bit
    Unknown
};

// ─── Classification label ────────────────────────────────────────────────────

enum class Label {
    Code,       ///< posterior > 0.7
    Data,       ///< posterior < 0.3
    Ambiguous   ///< 0.3 ≤ posterior ≤ 0.7  (flagged for manual review)
};

inline const char* labelName(Label l) noexcept
{
    switch (l) {
    case Label::Code:      return "code";
    case Label::Data:      return "data";
    case Label::Ambiguous: return "ambiguous";
    }
    return "unknown";
}

// ─── Reference type (evidence source 4) ──────────────────────────────────────

enum class RefType {
    Call,       ///< Direct CALL/BL → strong code evidence
    Jump,       ///< Direct JMP/B   → code evidence
    LoadImm,    ///< LEA/MOV-imm    → data evidence
    DataPtr,    ///< Pointer in a data section → data evidence
};

// ─── Per-byte alignment hint (evidence source 3) ─────────────────────────────

enum class AlignHint {
    FunctionEntry,    ///< 16-byte-aligned function entry (GCC/MSVC heuristic)
    BranchTarget,     ///< 4-byte-aligned branch target
    None
};

// ─── Per-address classification result ───────────────────────────────────────

struct ClassificationResult {
    uint64_t addr;          ///< Virtual address of the first byte of the region
    uint64_t size;          ///< Length of the contiguous region with same label
    Label    label;         ///< Code / Data / Ambiguous
    double   posterior;     ///< Final P(code) in [0,1]
};

// ─── CodeDataClassifier ───────────────────────────────────────────────────────

class CodeDataClassifier {
public:
    /**
     * Construct classifier for a binary image.
     *
     * @param arch      Target architecture.
     * @param imageBase Base virtual address of the image.
     * @param data      Raw image bytes (not owned; caller keeps alive).
     * @param size      Image byte count.
     */
    CodeDataClassifier(Arch           arch,
                       uint64_t       imageBase,
                       const uint8_t* data,
                       std::size_t    size);

    ~CodeDataClassifier() = default;

    // ── Evidence injection API ────────────────────────────────────────────────

    /// Mark [addr, addr+len) as reachable from the CFG.
    void addReachableRange(uint64_t addr, uint64_t len);

    /// Mark a single entry-point address (also adds reachable prior).
    void addEntryPoint(uint64_t addr);

    /// Report that a valid instruction of byte-length `instrLen` starts at addr.
    void addValidInstruction(uint64_t addr, uint32_t instrLen);

    /// Report that addr cannot be a valid instruction start (e.g. disassembler
    /// returned an error for every possible alignment).
    void addInvalidInstruction(uint64_t addr);

    /// Report that the instruction at addr has an impossible semantic (e.g.
    /// encoding that is UNDEFINED for the architecture).
    void addImpossibleSemantic(uint64_t addr);

    /// Add an alignment hint for addr.
    void addAlignmentHint(uint64_t addr, AlignHint hint);

    /// Add a reference to addr from some other location with the given type.
    void addReference(uint64_t targetAddr, RefType type);

    /**
     * Add an ARM/Thumb pointer.
     * If LSB of ptr is 1: the target (ptr & ~1) is Thumb code → code evidence.
     * If LSB of ptr is 0: the target is ARM code or data; treated as code if
     *   the byte at target looks like an ARM instruction start.
     */
    void addARMPointer(uint64_t ptr);

    /// Mark an executable address range (sections to classify).
    void addExecutableRange(uint64_t start, uint64_t end);

    // ── Classification ────────────────────────────────────────────────────────

    /**
     * Run the Bayesian classification over all registered addresses.
     * Must be called after all evidence has been injected.
     */
    void classify();

    // ── Result access ─────────────────────────────────────────────────────────

    /// Return the label for a single address. Returns Data if unknown.
    Label labelAt(uint64_t addr) const noexcept;

    /// Return the posterior P(code) for a single address. Returns 0.5 if unknown.
    double posteriorAt(uint64_t addr) const noexcept;

    /// Return true iff addr was classified as Thumb code (ARM only).
    bool isThumb(uint64_t addr) const noexcept;

    /**
     * Return a compacted list of contiguous regions, each with the same label.
     * Sorted by ascending address.
     */
    std::vector<ClassificationResult> results() const;

    /**
     * Return only CODE regions.
     */
    std::vector<ClassificationResult> codeRegions() const;

    /**
     * Return only DATA or AMBIGUOUS regions within executable sections.
     */
    std::vector<ClassificationResult> nonCodeRegions() const;

    // ── Statistics ────────────────────────────────────────────────────────────

    struct Stats {
        std::size_t totalBytes    = 0;
        std::size_t codeBytes     = 0;
        std::size_t dataBytes     = 0;
        std::size_t ambiguousBytes= 0;
    };
    Stats stats() const;

private:
    Arch           _arch;
    uint64_t       _imageBase;
    const uint8_t* _data;
    std::size_t    _size;

    // ── Internal per-address state ────────────────────────────────────────────

    /// Log-odds score per VMA.  Not present → prior (0.0 for unreachable).
    std::unordered_map<uint64_t, double> _logOdds;

    /// Whether the address is within a registered executable range.
    std::unordered_map<uint64_t, bool>   _execRange;

    /// Thumb interworking: addresses confirmed as Thumb code.
    std::unordered_map<uint64_t, bool>   _thumbSet;

    /// Classification cache (populated by classify()).
    mutable std::unordered_map<uint64_t, double> _posterior;
    mutable bool _classified = false;

    // ── Bayesian constants ────────────────────────────────────────────────────
    //
    // All log-likelihood-ratios (LLR) = log(P(ev|code) / P(ev|data)).
    // Positive → favours code; negative → favours data.

    // Evidence 1: Reachability
    static constexpr double kLLR_Reachable        =  2.944; // log(0.95/0.05)
    static constexpr double kLLR_Unreachable       = -0.847; // log(0.3/0.7)

    // Evidence 2: Instruction validity
    static constexpr double kLLR_ValidInstr        =  1.386; // log(0.8/0.2)
    static constexpr double kLLR_InvalidInstr      = -4.595; // log(0.01/0.99)
    static constexpr double kLLR_ImpossibleSemantic= -2.944; // log(0.05/0.95)

    // Evidence 3: Alignment
    static constexpr double kLLR_FuncAlign         =  1.099; // log(3/1) ≈ 3× more likely for code
    static constexpr double kLLR_BranchAlign       =  0.693; // log(2/1)

    // Evidence 4: Reference type
    static constexpr double kLLR_CallRef           =  3.892; // log(0.98/0.02)
    static constexpr double kLLR_JumpRef           =  2.944; // log(0.95/0.05)
    static constexpr double kLLR_LoadImmRef        = -1.386; // log(0.2/0.8)
    static constexpr double kLLR_DataPtrRef        = -1.386; // log(0.2/0.8)

    // Evidence 5: ARM Thumb pointer (LSB=1 → very strong code signal)
    static constexpr double kLLR_ThumbPtr          =  3.892; // log(0.98/0.02)
    static constexpr double kLLR_ARMPtr            =  1.386; // log(0.8/0.2)

    // Priors (in log-odds).
    static constexpr double kPrior_Reachable       =  2.197; // P(code)=0.9 → log(9)
    static constexpr double kPrior_Unreachable     =  0.0;   // P(code)=0.5

    // Classification thresholds.
    static constexpr double kCodeThreshold         =  0.7;
    static constexpr double kDataThreshold         =  0.3;

    // ── Helpers ───────────────────────────────────────────────────────────────
    double& logOddsAt(uint64_t addr);
    static double logOddsToProb(double lo) noexcept;
    bool inExecRange(uint64_t addr) const noexcept;
};

} // namespace code_data
} // namespace retdec

#endif // RETDEC_CODE_DATA_CODE_DATA_CLASSIFIER_H
