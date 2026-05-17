/**
 * @file include/retdec/fileformat/lattice/format_result.h
 * @brief Rich result from the Signature-Lattice file format parser.
 *
 * FormatResult captures everything downstream stages need:
 *  - detected format, architecture, endianness, address width
 *  - load bias (ASLR base)
 *  - flat section, import, export, and TLS tables
 *  - per-field plausibility flags for malformed-header recovery
 *  - AR-member sub-results for archive inputs
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace fileformat {
namespace lattice {

// ─── Enumerations ────────────────────────────────────────────────────────────

enum class DetectedFormat {
    Unknown,
    Raw,
    ELF32,
    ELF64,
    PE32,
    PE64,
    MachO32,
    MachO64,
    MachOFat,
    COFF,
    IntelHex,
    ARArchive,
};

enum class Arch {
    Unknown,
    X86,
    X86_64,
    ARM,
    AArch64,
    MIPS,
    MIPS64,
    PowerPC,
    PowerPC64,
    RISC_V,
};

enum class Endianness { Unknown, Little, Big };

// ─── Section ─────────────────────────────────────────────────────────────────

struct SectionInfo {
    std::string  name;
    uint64_t     virtualAddress = 0;
    uint64_t     virtualSize    = 0;
    uint64_t     fileOffset     = 0;
    uint64_t     fileSize       = 0;
    uint32_t     characteristics = 0; ///< PE section flags / ELF sh_flags
    bool         isExecutable   = false;
    bool         isWritable     = false;
    bool         isReadable     = true;
};

// ─── Import / Export ─────────────────────────────────────────────────────────

struct ImportEntry {
    std::string  functionName;
    std::string  moduleName;    ///< DLL name (PE) or library soname (ELF)
    uint64_t     address      = 0; ///< import thunk / GOT slot VA
    uint16_t     ordinal      = 0;
    bool         byOrdinal    = false;
};

struct ExportEntry {
    std::string  functionName;
    uint64_t     address      = 0;
    uint16_t     ordinal      = 0;
};

// ─── TLS ─────────────────────────────────────────────────────────────────────

struct TLSInfo {
    uint64_t              startAddressOfRawData = 0;
    uint64_t              endAddressOfRawData   = 0;
    uint64_t              addressOfIndex        = 0;
    std::vector<uint64_t> callbackAddresses;
};

// ─── Per-field corruption flags ───────────────────────────────────────────────

struct CorruptionFlags {
    bool entryPointOutOfRange = false;
    bool sectionCountSuspicious = false;
    bool strTableOffsetInvalid = false;
    bool importDirectoryInvalid = false;
    bool exportDirectoryInvalid = false;
    bool sectionOffsetsInvalid = false;

    bool any() const {
        return entryPointOutOfRange || sectionCountSuspicious ||
               strTableOffsetInvalid || importDirectoryInvalid ||
               exportDirectoryInvalid || sectionOffsetsInvalid;
    }
};

// ─── FormatResult ─────────────────────────────────────────────────────────────

struct FormatResult {
    DetectedFormat          format        = DetectedFormat::Unknown;
    Arch                    architecture  = Arch::Unknown;
    Endianness              endianness    = Endianness::Unknown;
    uint32_t                addressSize   = 0; ///< 32 or 64
    uint64_t                entryPoint    = 0;
    uint64_t                imageBase     = 0; ///< preferred load address
    uint64_t                loadBias      = 0; ///< difference from preferred

    std::vector<SectionInfo>  sections;
    std::vector<ImportEntry>  imports;
    std::vector<ExportEntry>  exports;
    std::optional<TLSInfo>    tls;

    CorruptionFlags           corruption;

    /// For AR archives: one result per member.
    std::vector<FormatResult> arMembers;

    /// Name / path (member name for AR members).
    std::string               name;

    bool isValid() const {
        return format != DetectedFormat::Unknown &&
               format != DetectedFormat::Raw;
    }

    bool is64Bit() const { return addressSize == 64; }
};

} // namespace lattice
} // namespace fileformat
} // namespace retdec
