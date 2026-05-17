/**
 * @file include/retdec/cli_parser/pe_reader.h
 * @brief PE/COFF header reader with .NET CLI directory support.
 *
 * ## PE Layout (relevant to .NET)
 *
 *   DOS header (0x00) → PE signature offset at 0x3C
 *   PE signature "PE\0\0"
 *   COFF FileHeader (20 bytes)
 *   Optional Header (PE32 or PE32+)
 *     Data Directories[14] = IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR
 *       → RVA + Size of the CLI header
 *   Section Table (NumberOfSections × 40 bytes)
 *   Section Data
 *
 * ## CLI Header (ECMA-335 §II.25.3.3)
 *
 *   DWORD  cb               // size of header
 *   WORD   MajorRuntimeVersion
 *   WORD   MinorRuntimeVersion
 *   IMAGE_DATA_DIRECTORY  MetaData      // RVA + Size
 *   DWORD  Flags
 *   DWORD  EntryPointToken  // or RVA for native entry
 *   IMAGE_DATA_DIRECTORY  Resources
 *   IMAGE_DATA_DIRECTORY  StrongNameSignature
 *   IMAGE_DATA_DIRECTORY  CodeManagerTable
 *   IMAGE_DATA_DIRECTORY  VTableFixups
 *   IMAGE_DATA_DIRECTORY  ExportAddressTableJumps
 *   IMAGE_DATA_DIRECTORY  ManagedNativeHeader
 *
 * ## Metadata root (§II.24.2.1)
 *
 *   DWORD  Signature   0x424A5342  "BSJB"
 *   WORD   MajorVersion, MinorVersion
 *   DWORD  Reserved
 *   DWORD  VersionLength (padded to 4 bytes)
 *   char   Version[VersionLength]
 *   WORD   Flags
 *   WORD   NumberOfStreams
 *   StreamHeader  Streams[NumberOfStreams]
 *     DWORD Offset, Size
 *     char  Name[padded to 4 bytes]
 *
 * This file implements RVA→file-offset translation and exposes:
 *   - Raw byte spans for each CLI metadata stream
 *   - Raw byte span for the CIL method bodies (via RVA resolution)
 *   - Assembly flags and entry-point token
 */

#ifndef RETDEC_CLI_PARSER_PE_READER_H
#define RETDEC_CLI_PARSER_PE_READER_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {

// ─── PE / COFF constants ─────────────────────────────────────────────────────

static constexpr uint32_t kPESignature      = 0x00004550u; // "PE\0\0"
static constexpr uint32_t kMDSignature      = 0x424A5342u; // "BSJB"
static constexpr uint16_t kMachineI386      = 0x014C;
static constexpr uint16_t kMachineAMD64     = 0x8664;
static constexpr uint16_t kMachineARM       = 0x01C0;
static constexpr uint16_t kMachineARM64     = 0xAA64;
static constexpr uint16_t kMachineIL        = 0x0000; // AnyCPU / MSIL
static constexpr uint16_t kOptPE32Magic     = 0x010B;
static constexpr uint16_t kOptPE32PlusMagic = 0x020B;
static constexpr int      kComDescriptorDir = 14; // IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR

// CLI header flags (ECMA-335 §II.25.3.3.1)
static constexpr uint32_t kCLIFlagILOnly        = 0x00000001;
static constexpr uint32_t kCLIFlag32BitRequired  = 0x00000002;
static constexpr uint32_t kCLIFlagStrongNameSigned = 0x00000008;
static constexpr uint32_t kCLIFlagNativeEntryPoint = 0x00000010;
static constexpr uint32_t kCLIFlagTrackDebugData   = 0x00010000;
static constexpr uint32_t kCLIFlag32BitPreferred    = 0x00020000;

// ─── Data structures ─────────────────────────────────────────────────────────

struct DataDirectory {
    uint32_t rva  = 0;
    uint32_t size = 0;
};

struct SectionHeader {
    char     name[8] = {};
    uint32_t virtualSize      = 0;
    uint32_t virtualAddress   = 0;  ///< RVA
    uint32_t rawDataSize      = 0;
    uint32_t rawDataOffset    = 0;  ///< File offset to raw data
    uint32_t relocationsOffset= 0;
    uint32_t lineNumbersOffset= 0;
    uint16_t relocationsCount = 0;
    uint16_t lineNumbersCount = 0;
    uint32_t characteristics  = 0;
};

struct CLIHeader {
    uint32_t      cb                        = 0;
    uint16_t      majorRuntimeVersion       = 0;
    uint16_t      minorRuntimeVersion       = 0;
    DataDirectory metaData;
    uint32_t      flags                     = 0;
    uint32_t      entryPointToken           = 0;   ///< metadata token or RVA
    DataDirectory resources;
    DataDirectory strongNameSignature;
    DataDirectory codeManagerTable;
    DataDirectory vTableFixups;
    DataDirectory exportAddressTableJumps;
    DataDirectory managedNativeHeader;
};

struct MetadataStreamHeader {
    uint32_t    offset = 0;   ///< Relative to start of metadata root
    uint32_t    size   = 0;
    std::string name;         ///< "#~", "#Strings", "#US", "#GUID", "#Blob"
};

// ─── PE reader ────────────────────────────────────────────────────────────────

/**
 * @brief Reads the PE/COFF structure of a .NET assembly.
 *
 * Works entirely from a byte span — zero-copy over an mmap'd or in-memory file.
 * Call `open()` once, then query the individual components.
 */
class PeReader {
public:
    PeReader() = default;

    /**
     * @brief Parse a PE image from a memory buffer.
     *
     * @param data   Pointer to raw file bytes (must remain valid for the
     *               lifetime of this PeReader).
     * @param size   File size in bytes.
     * @return       True on success.  On failure, `error()` is set.
     */
    bool open(const uint8_t* data, size_t size);

    bool isValid()    const { return valid_; }
    bool is64Bit()    const { return is64_; }
    bool hasCLI()     const { return hasCli_; }
    const std::string& error() const { return error_; }

    // Assembly identity
    std::string targetCpu() const;
    uint16_t    machine()   const { return machine_; }

    // CLI header
    const CLIHeader& cliHeader() const { return cliHeader_; }

    // Metadata streams (raw byte spans into the file buffer)
    std::span<const uint8_t> streamBytes(const std::string& name) const;
    const std::vector<MetadataStreamHeader>& streams() const { return streams_; }

    // The full metadata root span (from BSJB signature onwards)
    std::span<const uint8_t> metadataRoot() const { return metadataRoot_; }

    // CLR version string from metadata root (e.g. "v4.0.30319")
    const std::string& clrVersion() const { return clrVersion_; }

    // Resolve an RVA to a file offset.  Returns 0 if RVA not in any section.
    uint64_t rvaToOffset(uint32_t rva) const;

    // Resolve an RVA and return a span starting at that offset.
    std::span<const uint8_t> rvaToSpan(uint32_t rva, size_t maxSize = SIZE_MAX) const;

    // Sections
    const std::vector<SectionHeader>& sections() const { return sections_; }

    // Data directories
    const DataDirectory& comDescriptorDir() const { return comDir_; }

    // Raw file bytes
    const uint8_t* rawData()  const { return data_; }
    size_t         rawSize()  const { return size_; }

private:
    const uint8_t* data_ = nullptr;
    size_t         size_ = 0;

    bool        valid_   = false;
    bool        is64_    = false;
    bool        hasCli_  = false;
    uint16_t    machine_ = 0;
    std::string error_;
    std::string clrVersion_;

    CLIHeader                       cliHeader_;
    DataDirectory                   comDir_;
    std::vector<SectionHeader>      sections_;
    std::vector<MetadataStreamHeader> streams_;
    std::span<const uint8_t>        metadataRoot_;

    // File offset of the metadata root (BSJB).
    uint64_t metadataRootOffset_ = 0;

    // Internal parsing helpers
    bool parseCOFF(size_t peOffset);
    bool parseOptionalHeader(size_t optOffset, uint16_t magic);
    bool parseSections(size_t sectOffset, uint16_t count);
    bool parseCLIHeader();
    bool parseMetadataRoot();

    // Little-endian read helpers
    uint8_t  read8 (size_t off) const;
    uint16_t read16(size_t off) const;
    uint32_t read32(size_t off) const;
    uint64_t read64(size_t off) const;
    bool     checkRange(size_t off, size_t len) const;
};

} // namespace cli_parser
} // namespace retdec

#endif // RETDEC_CLI_PARSER_PE_READER_H
