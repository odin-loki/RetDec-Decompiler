/**
 * @file include/retdec/compiler_detect/compiler_fingerprinter.h
 * @brief CompilerFingerprinter — classify a binary's compiler from codegen patterns.
 *
 * Usage:
 *   CompilerFingerprinter fp;
 *   fp.addFunction(bytes, size);       // feed up to kMaxFunctions prologues
 *   fp.setRichHeader(vs_major);        // optional — from PE Rich header
 *   fp.setEHPersonality('g');          // 'g'=gxx, 'm'=MSVC, 0=none
 *   fp.setManglingStyle(itanium, msvc);
 *   fp.setImports(import_names);
 *   CompilerProfile p = fp.classify();
 *
 * All methods except classify() are feed-only and thread-safe between
 * different CompilerFingerprinter instances.  classify() must be called
 * from a single thread per instance.
 */

#ifndef RETDEC_COMPILER_DETECT_COMPILER_FINGERPRINTER_H
#define RETDEC_COMPILER_DETECT_COMPILER_FINGERPRINTER_H

#include "retdec/compiler_detect/compiler_profile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace compiler_detect {

class CompilerFingerprinter {
public:
    /// Maximum number of functions to analyse (first N in binary order).
    static constexpr uint32_t kMaxFunctions = 1000;

    CompilerFingerprinter() = default;

    // ── Feed methods ────────────────────────────────────────────────────────

    /**
     * Feed one function's raw machine code bytes.
     * At most kMaxFunctions calls are processed; subsequent calls are ignored.
     *
     * @param bytes  Pointer to function bytecode (x86-64).
     * @param size   Number of bytes.
     */
    void addFunction(const uint8_t* bytes, uint32_t size);

    /**
     * Provide the VS major version from the PE Rich header (MSVC-only).
     * Pass 0 if no Rich header is present.
     */
    void setRichHeader(uint32_t vsMajorVersion);

    /**
     * Provide the EH personality symbol character:
     *   'g' = __gxx_personality_v0   (GCC / Clang)
     *   'm' = __CxxFrameHandler3     (MSVC)
     *    0  = not detected
     */
    void setEHPersonality(char kind);

    /**
     * Tell the fingerprinter which name-mangling styles are present.
     * Itanium = _Z prefix (GCC/Clang).
     * MSVC    = ? prefix.
     */
    void setManglingStyle(bool hasItanium, bool hasMSVC);

    /**
     * Provide the list of imported symbol names (from IAT / .dynsym).
     * Used to detect __stack_chk_fail, __gxx_personality, etc.
     */
    void setImports(const std::vector<std::string>& importNames);

    // ── Classification ──────────────────────────────────────────────────────

    /**
     * Run the decision-tree classifier and return a CompilerProfile.
     * May be called multiple times (deterministic, no mutation after call).
     */
    CompilerProfile classify() const;

    /**
     * Expose the raw feature vector without full classification.
     * Useful for testing and diagnostics.
     */
    FeatureVector extractFeatures() const;

private:
    // ── Per-function prologue stats ─────────────────────────────────────────

    struct FuncStats {
        bool hasFramePointer    = false;  ///< RBP used as frame pointer
        bool hasTailCall        = false;  ///< exits via JMP (not RET)
        bool hasShadowSpace     = false;  ///< SUB RSP,32 or larger at call site
        bool hasStackAlign      = false;  ///< AND RSP,-16 / AND RSP,0xFFF...F0
        uint32_t memsetBytes    = 0;      ///< inlined memset size detected
    };

    std::vector<FuncStats> _funcs;
    uint32_t _richHeaderVSMajor = 0;
    char     _ehPersonality     = 0;
    bool     _itaniumMangling   = false;
    bool     _msvcMangling      = false;
    bool     _stackCanary       = false;

    // ── Feature extractors (called from extractFeatures) ────────────────────

    static FuncStats analysePrologue(const uint8_t* bytes, uint32_t size);

    // ── Decision tree ────────────────────────────────────────────────────────

    static CompilerProfile decisionTree(const FeatureVector& fv);

    // ── Version range helpers ────────────────────────────────────────────────

    static VersionRange gccVersionFromFeatures(const FeatureVector& fv);
    static VersionRange clangVersionFromFeatures(const FeatureVector& fv);
    static VersionRange msvcVersionFromFeatures(const FeatureVector& fv);
    static OptLevel     optLevelFromFeatures(const FeatureVector& fv,
                                              CompilerFamily family);
};

} // namespace compiler_detect
} // namespace retdec

#endif // RETDEC_COMPILER_DETECT_COMPILER_FINGERPRINTER_H
