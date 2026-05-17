/**
 * @file include/retdec/loader_sim/loader_sim.h
 * @brief OS Loader Simulation — ELF PLT/GOT, PE IAT, delay-load, TLS, ASLR.
 *
 * LoaderSim operates on a raw binary image (byte span + FormatResult from
 * Stage 1) and produces a LoadedImage that:
 *
 *   1. Applies base relocations (PE IMAGE_BASE_RELOCATION / ELF RELA/REL)
 *      so every absolute address in the image is valid.
 *   2. Resolves all imported symbols to named ImportRef structs.
 *   3. Marks PLT stubs (ELF) and IAT thunks (PE) as non-decompilable.
 *   4. Enumerates TLS callbacks (PE TLS directory, ELF .init_array) as
 *      synthetic entry points.
 *   5. Resolves delay-load imports (PE __delayLoadHelper2 / ImgDelayDescr).
 *
 * The class is format-agnostic at the API level; internally it dispatches
 * to ELF or PE sub-parsers based on the FormatResult supplied at construction.
 *
 * Usage:
 *   LoaderSim sim(data, size, formatResult);
 *   LoadedImage img = sim.load();
 *
 *   // Iterate resolved imports
 *   for (auto& imp : img.imports)
 *       printf("%08llx -> %s!%s\n", imp.vma, imp.dll.c_str(), imp.symbol.c_str());
 *
 *   // Check whether an address is a PLT/IAT stub
 *   if (img.isNonDecompilable(addr)) { ... }
 */

#ifndef RETDEC_LOADER_SIM_LOADER_SIM_H
#define RETDEC_LOADER_SIM_LOADER_SIM_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace loader_sim {

// ─── Forward declarations ─────────────────────────────────────────────────────

struct LoadedImage;

// ─── Section descriptor ───────────────────────────────────────────────────────

struct SectionDesc {
    std::string  name;
    uint64_t     vma       = 0;   ///< Virtual memory address (after rebasing)
    uint64_t     rawOffset = 0;   ///< Byte offset in the raw image buffer
    uint64_t     rawSize   = 0;   ///< Size in raw image
    uint64_t     virtSize  = 0;   ///< Virtual size (may differ from rawSize)
    bool         readable  = true;
    bool         writable  = false;
    bool         executable = false;
};

// ─── Resolved import reference ────────────────────────────────────────────────

struct ImportRef {
    uint64_t    vma;        ///< Address of the call target (PLT stub / IAT entry)
    std::string dll;        ///< DLL / shared-object name (lowercased)
    std::string symbol;     ///< Imported function/variable name (or ordinal string)
    uint32_t    ordinal = 0;///< Ordinal (0 = named import)
    bool        isDelayLoad = false; ///< true = delay-load import
};

// ─── TLS / init callback ─────────────────────────────────────────────────────

struct TLSCallback {
    uint64_t    vma;          ///< Address of the callback function
    std::string syntheticName;///< e.g. "__tls_callback_0"
};

// ─── Non-decompilable region ─────────────────────────────────────────────────

struct NonDecompilableRegion {
    uint64_t start; ///< First byte of the region (inclusive)
    uint64_t end;   ///< One past last byte (exclusive)
    std::string reason; ///< e.g. "PLT stub", "IAT thunk"
};

// ─── Relocation record ────────────────────────────────────────────────────────

struct RelocRecord {
    uint64_t vma;    ///< Address of the relocated field
    int32_t  addend; ///< Addend applied (for RELA; 0 for REL)
    uint8_t  type;   ///< Architecture-specific relocation type
};

// ─── LoadedImage ─────────────────────────────────────────────────────────────

struct LoadedImage {
    // ── Basic info ────────────────────────────────────────────────────────────
    uint64_t    imageBase       = 0;    ///< Preferred / actual load base
    uint64_t    loadBias        = 0;    ///< imageBase - preferredBase (ASLR delta)
    uint64_t    entryPoint      = 0;    ///< Rebased entry point

    // ── Memory layout ─────────────────────────────────────────────────────────
    std::vector<SectionDesc> sections;

    // ── Symbol resolution ─────────────────────────────────────────────────────
    std::vector<ImportRef>   imports;

    /// Map from import VMA → ImportRef index for O(1) lookup.
    std::unordered_map<uint64_t, std::size_t> importByVma;

    // ── TLS / init callbacks ─────────────────────────────────────────────────
    std::vector<TLSCallback> tlsCallbacks;

    // ── Non-decompilable regions ─────────────────────────────────────────────
    std::vector<NonDecompilableRegion> nonDecompilable;

    // ── Applied relocations ───────────────────────────────────────────────────
    std::vector<RelocRecord> relocations;

    // ── Errors / warnings accumulated during load ────────────────────────────
    std::vector<std::string> diagnostics;

    // ── Query helpers ─────────────────────────────────────────────────────────

    /// True if [addr] falls inside a non-decompilable region.
    bool isNonDecompilable(uint64_t addr) const noexcept;

    /// Resolve an import by exact VMA. Returns nullptr if not found.
    const ImportRef* resolveImport(uint64_t vma) const noexcept;

    /// Find the section that contains [addr], or nullptr.
    const SectionDesc* sectionAt(uint64_t addr) const noexcept;
};

// ─── LoaderSim ───────────────────────────────────────────────────────────────

class LoaderSim {
public:
    /**
     * Construct a loader simulation over a raw binary image.
     *
     * @param data      Pointer to image bytes (not owned; must remain valid).
     * @param size      Total byte count.
     * @param imageBase Preferred load base address (from format header).
     * @param is64Bit   True for 64-bit binaries; false for 32-bit.
     * @param isELF     True for ELF; false for PE/COFF.
     * @param isLE      True for little-endian; false for big-endian.
     */
    LoaderSim(const uint8_t* data,
              std::size_t    size,
              uint64_t       imageBase,
              bool           is64Bit,
              bool           isELF,
              bool           isLE = true);

    ~LoaderSim() = default;

    LoaderSim(const LoaderSim&)            = delete;
    LoaderSim& operator=(const LoaderSim&) = delete;

    /**
     * Run the full loader pipeline and return a LoadedImage.
     * The pipeline:
     *   1. Parse section table.
     *   2. Apply base relocations.
     *   3. Resolve imports (IAT / PLT).
     *   4. Resolve delay-load imports (PE only).
     *   5. Enumerate TLS / init callbacks.
     */
    LoadedImage load();

    // ── Pipeline step access (for testing / incremental use) ────────────────

    /// Parse section table only (step 1).
    std::vector<SectionDesc> parseSections() const;

    /// Apply PE base relocations. Returns list of patched records.
    std::vector<RelocRecord> applyPERelocations(uint64_t newBase) const;

    /// Apply ELF RELA/REL relocations from .rela.dyn / .rel.dyn.
    std::vector<RelocRecord> applyELFRelocations(uint64_t newBase) const;

    /// Resolve PE IAT imports.
    std::vector<ImportRef> resolvePEImports() const;

    /// Resolve ELF PLT imports via .rela.plt / .rel.plt.
    std::vector<ImportRef> resolveELFImports() const;

    /// Resolve PE delay-load imports.
    std::vector<ImportRef> resolvePEDelayImports() const;

    /// Enumerate PE TLS callbacks.
    std::vector<TLSCallback> parsePETLS() const;

    /// Enumerate ELF .init_array / .preinit_array callbacks.
    std::vector<TLSCallback> parseELFInitArray() const;

private:
    const uint8_t* _data      = nullptr;
    std::size_t    _size      = 0;
    uint64_t       _imageBase = 0;
    bool           _is64Bit   = false;
    bool           _isELF     = false;
    bool           _isLE      = true;

    // ── Raw memory read helpers ──────────────────────────────────────────────
    uint16_t r16(std::size_t off) const noexcept;
    uint32_t r32(std::size_t off) const noexcept;
    uint64_t r64(std::size_t off) const noexcept;
    uint64_t rPtr(std::size_t off) const noexcept; ///< r32 or r64 per _is64Bit

    /// Safe string read from null-terminated bytes at offset.
    std::string rStr(std::size_t off, std::size_t maxLen = 256) const;

    bool inBounds(std::size_t off, std::size_t len) const noexcept
    { return off < _size && len <= _size - off; }

    // ── ELF helpers ─────────────────────────────────────────────────────────
    struct ElfSection {
        std::string name;
        uint64_t    addr   = 0;
        uint64_t    offset = 0;
        uint64_t    size   = 0;
        uint32_t    type   = 0;
        uint64_t    entsize = 0;
        uint64_t    link   = 0;
        uint64_t    info   = 0;
        uint32_t    flags  = 0;
    };
    std::vector<ElfSection> parseElfSections() const;
    const ElfSection* findElfSection(const std::vector<ElfSection>& secs,
                                     const std::string& name) const;

    // ── PE helpers ───────────────────────────────────────────────────────────
    struct PESection {
        char     name[9] = {};
        uint64_t vma     = 0;
        uint64_t rawOff  = 0;
        uint32_t rawSize = 0;
        uint32_t virtSize = 0;
        uint32_t chars   = 0;
    };
    std::vector<PESection> parsePESections() const;
    uint64_t              peDataDir(int idx, bool& ok) const; ///< returns VA
    uint32_t              peDataDirSize(int idx) const;
    std::size_t           vaToOffset(uint64_t va,
                                     const std::vector<PESection>& secs) const;
};

} // namespace loader_sim
} // namespace retdec

#endif // RETDEC_LOADER_SIM_LOADER_SIM_H
