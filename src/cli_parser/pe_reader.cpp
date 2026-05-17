/**
 * @file src/cli_parser/pe_reader.cpp
 * @brief PE/COFF header reader with .NET CLI directory support.
 */

#include "retdec/cli_parser/pe_reader.h"

#include <algorithm>
#include <cstring>

namespace retdec {
namespace cli_parser {

// ─── Low-level helpers ────────────────────────────────────────────────────────

bool PeReader::checkRange(size_t off, size_t len) const {
    return off + len <= size_ && off + len >= off;
}

uint8_t PeReader::read8(size_t off) const {
    if (!checkRange(off, 1)) return 0;
    return data_[off];
}

uint16_t PeReader::read16(size_t off) const {
    if (!checkRange(off, 2)) return 0;
    return static_cast<uint16_t>(data_[off]) |
           (static_cast<uint16_t>(data_[off + 1]) << 8);
}

uint32_t PeReader::read32(size_t off) const {
    if (!checkRange(off, 4)) return 0;
    return static_cast<uint32_t>(data_[off])        |
           (static_cast<uint32_t>(data_[off + 1]) << 8)  |
           (static_cast<uint32_t>(data_[off + 2]) << 16) |
           (static_cast<uint32_t>(data_[off + 3]) << 24);
}

uint64_t PeReader::read64(size_t off) const {
    if (!checkRange(off, 8)) return 0;
    return static_cast<uint64_t>(read32(off)) |
           (static_cast<uint64_t>(read32(off + 4)) << 32);
}

// ─── PeReader::open ───────────────────────────────────────────────────────────

bool PeReader::open(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;
    valid_ = false;
    hasCli_ = false;

    if (size < 0x40) { error_ = "File too small for DOS header"; return false; }

    // DOS stub: check MZ signature
    if (read16(0) != 0x5A4D) { error_ = "Not a PE file (no MZ signature)"; return false; }

    // PE header offset stored at 0x3C
    uint32_t peOffset = read32(0x3C);
    if (!checkRange(peOffset, 4)) { error_ = "PE offset out of bounds"; return false; }
    if (read32(peOffset) != kPESignature) { error_ = "Invalid PE signature"; return false; }

    if (!parseCOFF(peOffset + 4)) return false;

    valid_ = true;
    return true;
}

bool PeReader::parseCOFF(size_t peOffset) {
    // COFF FileHeader is 20 bytes after PE signature
    if (!checkRange(peOffset, 20)) { error_ = "COFF header truncated"; return false; }

    machine_            = read16(peOffset);
    uint16_t numSect    = read16(peOffset + 2);
    uint16_t optHdrSize = read16(peOffset + 16);

    size_t optOffset = peOffset + 20;
    if (!checkRange(optOffset, optHdrSize)) { error_ = "Optional header truncated"; return false; }

    uint16_t magic = (optHdrSize >= 2) ? read16(optOffset) : 0;
    is64_ = (magic == kOptPE32PlusMagic);

    if (!parseOptionalHeader(optOffset, magic)) return false;

    size_t sectOffset = optOffset + optHdrSize;
    if (!parseSections(sectOffset, numSect)) return false;

    return true;
}

bool PeReader::parseOptionalHeader(size_t optOffset, uint16_t magic) {
    if (magic != kOptPE32Magic && magic != kOptPE32PlusMagic) {
        // ROM image or invalid — check if it might still have a valid structure
        if (magic == 0) { error_ = "No optional header"; return false; }
    }

    // Data directory array starts at fixed offsets:
    //   PE32:   optOffset + 96
    //   PE32+:  optOffset + 112
    size_t ddStart = optOffset + (is64_ ? 112 : 96);

    // COM descriptor directory is index 14
    size_t comOff = ddStart + kComDescriptorDir * 8;
    if (checkRange(comOff, 8)) {
        comDir_.rva  = read32(comOff);
        comDir_.size = read32(comOff + 4);
        hasCli_ = (comDir_.rva != 0 && comDir_.size >= 72);
    }

    return true;
}

bool PeReader::parseSections(size_t sectOffset, uint16_t count) {
    sections_.clear();
    sections_.reserve(count);

    for (uint16_t i = 0; i < count; ++i) {
        size_t base = sectOffset + i * 40;
        if (!checkRange(base, 40)) { error_ = "Section header truncated"; return false; }

        SectionHeader s;
        std::memcpy(s.name, data_ + base, 8);
        s.virtualSize    = read32(base + 8);
        s.virtualAddress = read32(base + 12);
        s.rawDataSize    = read32(base + 16);
        s.rawDataOffset  = read32(base + 20);
        sections_.push_back(s);
    }

    if (hasCli_) {
        if (!parseCLIHeader()) return false;
        if (!parseMetadataRoot()) return false;
    }

    return true;
}

uint64_t PeReader::rvaToOffset(uint32_t rva) const {
    for (const auto& s : sections_) {
        if (rva >= s.virtualAddress &&
            rva < s.virtualAddress + std::max(s.virtualSize, s.rawDataSize)) {
            return s.rawDataOffset + (rva - s.virtualAddress);
        }
    }
    return 0;
}

std::span<const uint8_t> PeReader::rvaToSpan(uint32_t rva, size_t maxSize) const {
    uint64_t off = rvaToOffset(rva);
    if (off == 0 || off >= size_) return {};
    size_t available = size_ - static_cast<size_t>(off);
    size_t len = std::min(available, maxSize);
    return {data_ + off, len};
}

bool PeReader::parseCLIHeader() {
    uint64_t cliOff = rvaToOffset(comDir_.rva);
    if (cliOff == 0 || !checkRange(cliOff, 72)) {
        error_ = "CLI header not found or truncated";
        hasCli_ = false;
        return true;  // Non-fatal: might be unmanaged PE
    }

    cliHeader_.cb                  = read32(cliOff + 0);
    cliHeader_.majorRuntimeVersion = read16(cliOff + 4);
    cliHeader_.minorRuntimeVersion = read16(cliOff + 6);
    cliHeader_.metaData.rva        = read32(cliOff + 8);
    cliHeader_.metaData.size       = read32(cliOff + 12);
    cliHeader_.flags               = read32(cliOff + 16);
    cliHeader_.entryPointToken     = read32(cliOff + 20);
    cliHeader_.resources.rva       = read32(cliOff + 24);
    cliHeader_.resources.size      = read32(cliOff + 28);
    cliHeader_.strongNameSignature.rva  = read32(cliOff + 32);
    cliHeader_.strongNameSignature.size = read32(cliOff + 36);

    return true;
}

bool PeReader::parseMetadataRoot() {
    if (!cliHeader_.metaData.rva) return true;

    uint64_t mdOff = rvaToOffset(cliHeader_.metaData.rva);
    if (mdOff == 0 || !checkRange(mdOff, 16)) {
        error_ = "Metadata root truncated";
        return false;
    }

    if (read32(mdOff) != kMDSignature) {
        error_ = "Invalid metadata signature (expected BSJB)";
        return false;
    }

    metadataRootOffset_ = mdOff;

    uint32_t versionLength = read32(mdOff + 12);
    // VersionLength is padded to 4-byte boundary
    versionLength = (versionLength + 3) & ~3u;

    size_t versionStart = mdOff + 16;
    if (!checkRange(versionStart, versionLength)) {
        error_ = "Metadata version string truncated";
        return false;
    }

    // Version string is null-terminated within versionLength bytes
    const char* ver = reinterpret_cast<const char*>(data_ + versionStart);
    clrVersion_ = std::string(ver, std::min(versionLength,
                               static_cast<uint32_t>(
                                   std::strlen(ver))));

    // After version: Flags(2) + NumberOfStreams(2)
    size_t hdrAfterVer = versionStart + versionLength;
    if (!checkRange(hdrAfterVer, 4)) {
        error_ = "Metadata stream count truncated";
        return false;
    }

    uint16_t numStreams = read16(hdrAfterVer + 2);

    // Parse stream headers
    size_t streamHdrOff = hdrAfterVer + 4;
    streams_.clear();
    streams_.reserve(numStreams);

    for (uint16_t i = 0; i < numStreams; ++i) {
        if (!checkRange(streamHdrOff, 8)) break;

        MetadataStreamHeader sh;
        sh.offset = read32(streamHdrOff);
        sh.size   = read32(streamHdrOff + 4);

        // Name: null-terminated, padded to 4 bytes
        size_t nameOff = streamHdrOff + 8;
        const char* namePtr = reinterpret_cast<const char*>(data_ + nameOff);
        size_t nameLen = 0;
        while (nameOff + nameLen < size_ && namePtr[nameLen] != '\0') ++nameLen;
        sh.name = std::string(namePtr, nameLen);
        size_t paddedNameLen = (nameLen + 4) & ~3u;

        streams_.push_back(sh);
        streamHdrOff = nameOff + paddedNameLen;
    }

    // Build metadataRoot_ span
    if (cliHeader_.metaData.size > 0 && checkRange(mdOff, cliHeader_.metaData.size))
        metadataRoot_ = {data_ + mdOff, cliHeader_.metaData.size};

    return true;
}

std::span<const uint8_t> PeReader::streamBytes(const std::string& name) const {
    for (const auto& sh : streams_) {
        if (sh.name == name) {
            uint64_t off = metadataRootOffset_ + sh.offset;
            if (off < size_ && off + sh.size <= size_)
                return {data_ + off, sh.size};
        }
    }
    return {};
}

std::string PeReader::targetCpu() const {
    switch (machine_) {
        case kMachineI386:  return "x86";
        case kMachineAMD64: return "x86_64";
        case kMachineARM:   return "arm";
        case kMachineARM64: return "aarch64";
        case kMachineIL:    return "msil";
        default:            return "unknown";
    }
}

} // namespace cli_parser
} // namespace retdec
