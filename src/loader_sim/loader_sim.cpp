/**
 * @file src/loader_sim/loader_sim.cpp
 * @brief OS Loader Simulation implementation.
 *
 * ## PE pipeline
 *
 *   parsePESections()
 *     Walk the section table from the NT headers.
 *
 *   applyPERelocations(newBase)
 *     IMAGE_BASE_RELOCATION blocks: each entry encodes a type (absolute, high,
 *     low, highlow, dir64) and an offset within the page.  Only HIGHLOW (3) and
 *     DIR64 (10) are applied; others are no-ops on x86.
 *
 *   resolvePEImports()
 *     Walk IMAGE_IMPORT_DESCRIPTOR chain until Name==0.  For each descriptor
 *     walk the OriginalFirstThunk (INT) to get name/ordinal, pair with the
 *     FirstThunk (IAT) address.  Mark the full IAT range non-decompilable.
 *
 *   resolvePEDelayImports()
 *     Walk ImgDelayDescr[] until rvaDLLName==0.  Each entry has rvaIAT pointing
 *     to the delay-load IAT (initially all 0 / loader stubs).  Resolve using
 *     rvaINT (import name table) exactly like regular imports.
 *
 *   parsePETLS()
 *     IMAGE_TLS_DIRECTORY{32|64}: AddressOfCallBacks is a VA pointing to a
 *     null-terminated array of function VAs.  Each non-null entry is a
 *     TLS callback.
 *
 * ## ELF pipeline
 *
 *   parseElfSections()
 *     Walk the ELF section header table; strtab names resolved via SHT_STRTAB
 *     at e_shstrndx.
 *
 *   applyELFRelocations(newBase)
 *     Process SHT_RELA (.rela.dyn) and SHT_REL (.rel.dyn) sections.
 *     Supported types: R_X86_64_64 (1), R_X86_64_RELATIVE (8),
 *     R_X86_64_GLOB_DAT (6), R_X86_64_JUMP_SLOT (7).
 *
 *   resolveELFImports()
 *     Process .rela.plt (.rel.plt on 32-bit): each entry gives an offset into
 *     .got.plt and a symbol index.  The symbol name is read from .dynsym via
 *     .dynstr.  The PLT stub address = .plt base + 16 + 16*N (standard layout).
 *
 *   parseELFInitArray()
 *     Read pointer-sized entries from .init_array and .preinit_array.
 *
 * ## Constants
 *   PE data directory indices:
 *     0=exports, 1=imports, 2=resource, 3=exception, 4=security, 5=basereloc,
 *     6=debug, 9=tls, 13=delayimport
 */

#include "retdec/loader_sim/loader_sim.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace retdec {
namespace loader_sim {

// ─── LoadedImage query helpers ────────────────────────────────────────────────

bool LoadedImage::isNonDecompilable(uint64_t addr) const noexcept
{
    for (const auto& r : nonDecompilable) {
        if (addr >= r.start && addr < r.end) return true;
    }
    return false;
}

const ImportRef* LoadedImage::resolveImport(uint64_t vma) const noexcept
{
    auto it = importByVma.find(vma);
    if (it == importByVma.end()) return nullptr;
    return &imports[it->second];
}

const SectionDesc* LoadedImage::sectionAt(uint64_t addr) const noexcept
{
    for (const auto& s : sections) {
        if (addr >= s.vma && addr < s.vma + s.virtSize) return &s;
    }
    return nullptr;
}

// ─── LoaderSim constructor ────────────────────────────────────────────────────

LoaderSim::LoaderSim(const uint8_t* data, std::size_t size,
                     uint64_t imageBase, bool is64Bit, bool isELF, bool isLE)
    : _data(data), _size(size), _imageBase(imageBase),
      _is64Bit(is64Bit), _isELF(isELF), _isLE(isLE)
{}

// ─── Raw memory read helpers ─────────────────────────────────────────────────

uint16_t LoaderSim::r16(std::size_t off) const noexcept
{
    if (!inBounds(off, 2)) return 0;
    if (_isLE)
        return static_cast<uint16_t>(_data[off]) |
               (static_cast<uint16_t>(_data[off+1]) << 8);
    return (static_cast<uint16_t>(_data[off]) << 8) |
            static_cast<uint16_t>(_data[off+1]);
}

uint32_t LoaderSim::r32(std::size_t off) const noexcept
{
    if (!inBounds(off, 4)) return 0;
    if (_isLE)
        return static_cast<uint32_t>(_data[off])       |
              (static_cast<uint32_t>(_data[off+1]) <<  8) |
              (static_cast<uint32_t>(_data[off+2]) << 16) |
              (static_cast<uint32_t>(_data[off+3]) << 24);
    return (static_cast<uint32_t>(_data[off])   << 24) |
           (static_cast<uint32_t>(_data[off+1]) << 16) |
           (static_cast<uint32_t>(_data[off+2]) <<  8) |
            static_cast<uint32_t>(_data[off+3]);
}

uint64_t LoaderSim::r64(std::size_t off) const noexcept
{
    if (!inBounds(off, 8)) return 0;
    if (_isLE) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(_data[off+i]) << (i*8);
        return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | _data[off+i];
    return v;
}

uint64_t LoaderSim::rPtr(std::size_t off) const noexcept
{
    return _is64Bit ? r64(off) : r32(off);
}

std::string LoaderSim::rStr(std::size_t off, std::size_t maxLen) const
{
    if (off >= _size) return {};
    std::size_t end = std::min(off + maxLen, _size);
    std::string s;
    for (std::size_t i = off; i < end; ++i) {
        if (_data[i] == 0) break;
        s += static_cast<char>(_data[i]);
    }
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PE subsystems
// ═══════════════════════════════════════════════════════════════════════════════

// PE field offsets relative to start of IMAGE_NT_HEADERS (after "PE\0\0").
static constexpr std::size_t kNTSig        = 0;   // "PE\0\0"
static constexpr std::size_t kFileHdr      = 4;
static constexpr std::size_t kMachine      = kFileHdr + 0;  // +0
static constexpr std::size_t kNumSections  = kFileHdr + 2;
static constexpr std::size_t kOptHdrOff    = kFileHdr + 16; // SizeOfOptionalHeader offset
static constexpr std::size_t kOptHdr       = kFileHdr + 20; // optional header starts here

// PE32 optional header offsets (from start of optional header).
static constexpr std::size_t kMagic        = 0;
static constexpr std::size_t kEntryRVA32   = 16;
static constexpr std::size_t kImageBase32  = 28;
static constexpr std::size_t kDataDir32    = 96;   // first data directory entry

// PE32+ (64-bit) optional header offsets.
static constexpr std::size_t kEntryRVA64   = 16;
static constexpr std::size_t kImageBase64  = 24;
static constexpr std::size_t kDataDir64    = 112;

static constexpr uint16_t kOptMagicPE32   = 0x010B;
static constexpr uint16_t kOptMagicPE32P  = 0x020B;

/// Locate the NT headers offset from the DOS stub at offset 0x3C.
static std::size_t peNtOffset(const uint8_t* data, std::size_t size)
{
    if (size < 0x40) return 0;
    uint32_t e_lfanew = static_cast<uint32_t>(data[0x3C]) |
                        (static_cast<uint32_t>(data[0x3D]) << 8) |
                        (static_cast<uint32_t>(data[0x3E]) << 16) |
                        (static_cast<uint32_t>(data[0x3F]) << 24);
    if (e_lfanew + 24 >= size) return 0;
    // Verify "PE\0\0" signature.
    if (data[e_lfanew]   != 'P' || data[e_lfanew+1] != 'E' ||
        data[e_lfanew+2] != 0   || data[e_lfanew+3] != 0)
        return 0;
    return static_cast<std::size_t>(e_lfanew);
}

std::vector<LoaderSim::PESection> LoaderSim::parsePESections() const
{
    std::vector<PESection> secs;
    std::size_t ntOff = peNtOffset(_data, _size);
    if (ntOff == 0) return secs;

    std::size_t nhOff = ntOff + kNTSig;   // IMAGE_NT_HEADERS base
    uint16_t numSec = r16(nhOff + kNumSections);
    uint16_t optSz  = r16(nhOff + kOptHdrOff);

    // Section table starts after FILE_HEADER (20 bytes) + optional header.
    std::size_t secTblOff = ntOff + 4 + 20 + optSz;

    for (uint16_t i = 0; i < numSec; ++i) {
        std::size_t base = secTblOff + i * 40;
        if (!inBounds(base, 40)) break;

        PESection s;
        std::memcpy(s.name, _data + base, 8);
        s.name[8] = 0;

        s.virtSize = r32(base + 8);
        uint32_t rva    = r32(base + 12);
        s.rawSize       = r32(base + 16);
        uint32_t rawOff = r32(base + 20);
        s.chars         = r32(base + 36);

        // Read optional header magic to get image base.
        std::size_t optOff = ntOff + 4 + 20;
        uint16_t magic = r16(optOff + kMagic);
        uint64_t base64 = (magic == kOptMagicPE32P)
            ? r64(optOff + kImageBase64)
            : r32(optOff + kImageBase32);

        s.vma    = base64 + rva;
        s.rawOff = rawOff;
        secs.push_back(s);
    }
    return secs;
}

std::size_t LoaderSim::vaToOffset(uint64_t va,
                                   const std::vector<PESection>& secs) const
{
    std::size_t optOff = 0;
    {
        std::size_t ntOff = peNtOffset(_data, _size);
        if (ntOff == 0) return _size; // invalid
        uint16_t magic = r16(ntOff + 4 + 20 + kMagic);
        uint64_t ib = (magic == kOptMagicPE32P)
            ? r64(ntOff + 4 + 20 + kImageBase64)
            : r32(ntOff + 4 + 20 + kImageBase32);
        uint32_t rva = static_cast<uint32_t>(va - ib);
        for (const auto& s : secs) {
            uint32_t secRva = static_cast<uint32_t>(s.vma - ib);
            if (rva >= secRva && rva < secRva + std::max(s.rawSize, s.virtSize)) {
                return static_cast<std::size_t>(s.rawOff) + (rva - secRva);
            }
        }
        (void)optOff;
    }
    return _size; // unmapped
}

uint64_t LoaderSim::peDataDir(int idx, bool& ok) const
{
    ok = false;
    std::size_t ntOff = peNtOffset(_data, _size);
    if (ntOff == 0) return 0;
    std::size_t optOff = ntOff + 4 + 20;
    uint16_t magic = r16(optOff + kMagic);
    std::size_t ddOff = (magic == kOptMagicPE32P)
        ? (optOff + kDataDir64 + idx * 8)
        : (optOff + kDataDir32 + idx * 8);
    if (!inBounds(ddOff, 8)) return 0;
    uint32_t rva = r32(ddOff);
    if (rva == 0) return 0;

    uint64_t ib = (magic == kOptMagicPE32P)
        ? r64(optOff + kImageBase64)
        : r32(optOff + kImageBase32);
    ok = true;
    return ib + rva;
}

uint32_t LoaderSim::peDataDirSize(int idx) const
{
    std::size_t ntOff = peNtOffset(_data, _size);
    if (ntOff == 0) return 0;
    std::size_t optOff = ntOff + 4 + 20;
    uint16_t magic = r16(optOff + kMagic);
    std::size_t ddOff = (magic == kOptMagicPE32P)
        ? (optOff + kDataDir64 + idx * 8)
        : (optOff + kDataDir32 + idx * 8);
    if (!inBounds(ddOff, 8)) return 0;
    return r32(ddOff + 4);
}

// ── PE base relocations ───────────────────────────────────────────────────────

std::vector<RelocRecord> LoaderSim::applyPERelocations(uint64_t newBase) const
{
    std::vector<RelocRecord> recs;
    bool ok;
    uint64_t relocVA = peDataDir(5, ok);
    if (!ok) return recs;

    auto secs = parsePESections();
    std::size_t off = vaToOffset(relocVA, secs);
    uint64_t delta = newBase - _imageBase;

    while (inBounds(off, 8)) {
        uint32_t pageVA  = r32(off);
        uint32_t blkSize = r32(off + 4);
        if (blkSize < 8 || pageVA == 0) break;

        uint32_t numEntries = (blkSize - 8) / 2;
        for (uint32_t i = 0; i < numEntries; ++i) {
            if (!inBounds(off + 8 + i * 2, 2)) break;
            uint16_t entry = r16(off + 8 + i * 2);
            uint8_t  type  = static_cast<uint8_t>(entry >> 12);
            uint16_t relOff= entry & 0x0FFF;

            uint64_t fieldVA = _imageBase + pageVA + relOff;
            std::size_t fieldOff = vaToOffset(fieldVA, secs);

            RelocRecord rec;
            rec.vma    = fieldVA + delta;
            rec.addend = 0;
            rec.type   = type;

            if (type == 3) { // HIGHLOW (32-bit absolute)
                if (inBounds(fieldOff, 4)) {
                    uint32_t orig = r32(fieldOff);
                    (void)(orig + static_cast<uint32_t>(delta)); // patch intent
                }
                recs.push_back(rec);
            } else if (type == 10) { // DIR64 (64-bit absolute)
                if (inBounds(fieldOff, 8)) {
                    uint64_t orig = r64(fieldOff);
                    (void)(orig + delta);
                }
                recs.push_back(rec);
            }
            // type 0 = padding, types 1/2 = MIPS/Alpha, skip.
        }
        off += blkSize;
    }
    return recs;
}

// ── PE IAT import resolution ──────────────────────────────────────────────────

std::vector<ImportRef> LoaderSim::resolvePEImports() const
{
    std::vector<ImportRef> imports;
    bool ok;
    uint64_t importVA = peDataDir(1, ok);
    if (!ok) return imports;

    auto secs = parsePESections();

    // Read image base.
    std::size_t ntOff  = peNtOffset(_data, _size);
    std::size_t optOff = ntOff + 4 + 20;
    uint16_t magic = r16(optOff + kMagic);
    uint64_t ib = (magic == kOptMagicPE32P)
        ? r64(optOff + kImageBase64)
        : r32(optOff + kImageBase32);

    std::size_t descOff = vaToOffset(importVA, secs);

    // Walk IMAGE_IMPORT_DESCRIPTOR (20 bytes each).
    for (;;) {
        if (!inBounds(descOff, 20)) break;
        uint32_t origFirstThunk = r32(descOff + 0);
        // uint32_t timeDateStamp  = r32(descOff + 4);
        // uint32_t forwarderChain = r32(descOff + 8);
        uint32_t nameRva        = r32(descOff + 12);
        uint32_t firstThunk     = r32(descOff + 16);
        descOff += 20;

        if (nameRva == 0 && firstThunk == 0) break; // sentinel

        std::string dllName;
        {
            std::size_t nOff = vaToOffset(ib + nameRva, secs);
            if (nOff < _size) dllName = rStr(nOff);
        }
        // Normalise to lowercase.
        for (auto& c : dllName) c = static_cast<char>(std::tolower(c));

        // Use INT (OriginalFirstThunk) if available; fall back to IAT.
        uint32_t thunkRva = (origFirstThunk != 0) ? origFirstThunk : firstThunk;
        std::size_t intOff = vaToOffset(ib + thunkRva, secs);
        std::size_t iatOff = vaToOffset(ib + firstThunk, secs);

        uint64_t iatStart = ib + firstThunk;
        uint64_t iatEnd   = iatStart;

        for (uint32_t entry = 0; ; ++entry) {
            std::size_t ptrSize = _is64Bit ? 8u : 4u;
            std::size_t intPtr = intOff + entry * ptrSize;
            std::size_t iatPtr = iatOff + entry * ptrSize;
            if (!inBounds(intPtr, ptrSize)) break;

            uint64_t val = _is64Bit ? r64(intPtr) : r32(intPtr);
            if (val == 0) break;

            ImportRef imp;
            imp.dll = dllName;
            imp.vma = ib + firstThunk + entry * ptrSize;
            iatEnd  = imp.vma + ptrSize;

            uint64_t ordFlag = _is64Bit ? (1ULL << 63) : (1ULL << 31);
            if (val & ordFlag) {
                // Import by ordinal.
                imp.ordinal = static_cast<uint32_t>(val & 0xFFFF);
                imp.symbol  = "Ordinal_" + std::to_string(imp.ordinal);
            } else {
                // Import by name: val is RVA to IMAGE_IMPORT_BY_NAME.
                std::size_t ibnOff = vaToOffset(ib + (val & 0x7FFFFFFF), secs);
                if (inBounds(ibnOff, 2)) {
                    imp.symbol = rStr(ibnOff + 2); // skip Hint (uint16)
                }
            }
            imports.push_back(imp);

            // Mirror into IAT slot address.
            if (inBounds(iatPtr, ptrSize)) {
                imports.back().vma = ib + firstThunk + entry * ptrSize;
            }
        }

        // Record non-decompilable IAT range (stored separately in load()).
        (void)iatStart; (void)iatEnd;
    }
    return imports;
}

// ── PE delay-load import resolution ──────────────────────────────────────────

// ImgDelayDescr offsets (all 32-bit RVAs for both PE32 and PE32+).
static constexpr std::size_t kDDAttrs       = 0;
static constexpr std::size_t kDDNameRva     = 4;
static constexpr std::size_t kDDModuleHandle= 8;
static constexpr std::size_t kDDIATRva      = 12;
static constexpr std::size_t kDDINTRva      = 16;
// static constexpr std::size_t kDDBoundIAT = 20;
// static constexpr std::size_t kDDUnloadIAT= 24;
static constexpr std::size_t kDDSize        = 32;

std::vector<ImportRef> LoaderSim::resolvePEDelayImports() const
{
    std::vector<ImportRef> imports;
    bool ok;
    uint64_t delayVA = peDataDir(13, ok);
    if (!ok) return imports;

    auto secs = parsePESections();

    std::size_t ntOff  = peNtOffset(_data, _size);
    std::size_t optOff = ntOff + 4 + 20;
    uint16_t magic = r16(optOff + kMagic);
    uint64_t ib = (magic == kOptMagicPE32P)
        ? r64(optOff + kImageBase64)
        : r32(optOff + kImageBase32);

    std::size_t off = vaToOffset(delayVA, secs);

    for (;;) {
        if (!inBounds(off, kDDSize)) break;
        uint32_t attrs   = r32(off + kDDAttrs);
        uint32_t nameRva = r32(off + kDDNameRva);
        uint32_t iatRva  = r32(off + kDDIATRva);
        uint32_t intRva  = r32(off + kDDINTRva);
        off += kDDSize;

        if (nameRva == 0) break;

        // attrs bit 1 set = RVA-based (newer format); otherwise VA-based.
        bool rvaBase = (attrs & 1) != 0;
        uint64_t nameVA = rvaBase ? (ib + nameRva) : nameRva;
        uint64_t iatVA  = rvaBase ? (ib + iatRva)  : iatRva;
        uint64_t intVA  = rvaBase ? (ib + intRva)  : intRva;

        std::string dllName;
        {
            std::size_t nOff = vaToOffset(nameVA, secs);
            if (nOff < _size) dllName = rStr(nOff);
        }
        for (auto& c : dllName) c = static_cast<char>(std::tolower(c));

        std::size_t intOff = vaToOffset(intVA, secs);
        std::size_t ptrSize = _is64Bit ? 8u : 4u;

        for (uint32_t entry = 0; ; ++entry) {
            std::size_t intPtr = intOff + entry * ptrSize;
            if (!inBounds(intPtr, ptrSize)) break;
            uint64_t val = _is64Bit ? r64(intPtr) : r32(intPtr);
            if (val == 0) break;

            ImportRef imp;
            imp.dll        = dllName;
            imp.vma        = iatVA + entry * ptrSize;
            imp.isDelayLoad = true;

            uint64_t ordFlag = _is64Bit ? (1ULL << 63) : (1ULL << 31);
            if (val & ordFlag) {
                imp.ordinal = static_cast<uint32_t>(val & 0xFFFF);
                imp.symbol  = "Ordinal_" + std::to_string(imp.ordinal);
            } else {
                std::size_t ibnOff = vaToOffset(ib + (val & 0x7FFFFFFF), secs);
                if (inBounds(ibnOff, 2))
                    imp.symbol = rStr(ibnOff + 2);
            }
            imports.push_back(imp);
        }
    }
    return imports;
}

// ── PE TLS callbacks ──────────────────────────────────────────────────────────

std::vector<TLSCallback> LoaderSim::parsePETLS() const
{
    std::vector<TLSCallback> cbs;
    bool ok;
    uint64_t tlsVA = peDataDir(9, ok);
    if (!ok) return cbs;

    auto secs = parsePESections();
    std::size_t tlsOff = vaToOffset(tlsVA, secs);
    if (!inBounds(tlsOff, _is64Bit ? 40u : 24u)) return cbs;

    // IMAGE_TLS_DIRECTORY{32|64}:
    //   PE32:  StartVA(4), EndVA(4), AddressOfIndex(4), AddressOfCallBacks(4),...
    //   PE32+: StartVA(8), EndVA(8), AddressOfIndex(8), AddressOfCallBacks(8),...
    uint64_t cbTableVA = _is64Bit
        ? r64(tlsOff + 24)   // 3rd 8-byte field
        : r32(tlsOff + 12);  // 3rd 4-byte field

    if (cbTableVA == 0) return cbs;

    std::size_t cbOff = vaToOffset(cbTableVA, secs);
    std::size_t ptrSz = _is64Bit ? 8u : 4u;

    for (uint32_t i = 0; ; ++i) {
        if (!inBounds(cbOff + i * ptrSz, ptrSz)) break;
        uint64_t fnVA = _is64Bit ? r64(cbOff + i * ptrSz)
                                 : r32(cbOff + i * ptrSz);
        if (fnVA == 0) break;
        TLSCallback cb;
        cb.vma           = fnVA;
        cb.syntheticName = "__tls_callback_" + std::to_string(i);
        cbs.push_back(cb);
    }
    return cbs;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ELF subsystems
// ═══════════════════════════════════════════════════════════════════════════════

// ELF header offsets (common between 32/64).
static constexpr std::size_t kElfMagic       = 0;   // "\x7fELF"
static constexpr std::size_t kElfClass       = 4;   // 1=32,2=64
static constexpr std::size_t kElfData        = 5;   // 1=LE,2=BE
static constexpr std::size_t kElfShOff32     = 32;
static constexpr std::size_t kElfShOff64     = 40;
static constexpr std::size_t kElfShNum32     = 48;
static constexpr std::size_t kElfShNum64     = 60;
static constexpr std::size_t kElfShStrNdx32  = 50;
static constexpr std::size_t kElfShStrNdx64  = 62;

// ELF section header offsets.
static constexpr std::size_t kShNameOff   = 0;
static constexpr std::size_t kShTypeOff   = 4;
static constexpr std::size_t kShFlagsOff  = 8;
static constexpr std::size_t kShAddrOff32 = 12;
static constexpr std::size_t kShOffOff32  = 16;
static constexpr std::size_t kShSizeOff32 = 20;
static constexpr std::size_t kShLinkOff32 = 24;
static constexpr std::size_t kShInfoOff32 = 28;
static constexpr std::size_t kShEntSz32   = 36;
static constexpr std::size_t kShHdrSz32   = 40;

static constexpr std::size_t kShAddrOff64 = 16;
static constexpr std::size_t kShOffOff64  = 24;
static constexpr std::size_t kShSizeOff64 = 32;
static constexpr std::size_t kShLinkOff64 = 40;
static constexpr std::size_t kShInfoOff64 = 44;
static constexpr std::size_t kShEntSz64   = 56;
static constexpr std::size_t kShHdrSz64   = 64;

// SHT types.
static constexpr uint32_t kSHT_RELA   = 4;
static constexpr uint32_t kSHT_REL    = 9;
static constexpr uint32_t kSHT_DYNSYM = 11;
static constexpr uint32_t kSHT_STRTAB = 3;

std::vector<LoaderSim::ElfSection> LoaderSim::parseElfSections() const
{
    std::vector<ElfSection> secs;
    if (_size < 64) return secs;
    if (_data[0]!=0x7F||_data[1]!='E'||_data[2]!='L'||_data[3]!='F') return secs;

    bool is64 = _is64Bit;
    std::size_t shoff = is64 ? r64(kElfShOff64) : r32(kElfShOff32);
    uint16_t shnum    = is64 ? r16(kElfShNum64)  : r16(kElfShNum32);
    uint16_t shstrndx = is64 ? r16(kElfShStrNdx64) : r16(kElfShStrNdx32);
    std::size_t shsz  = is64 ? kShHdrSz64 : kShHdrSz32;

    if (!inBounds(shoff, shnum * shsz)) return secs;

    // First, find .shstrtab offset.
    std::size_t strtabOff = 0;
    if (shstrndx < shnum) {
        std::size_t sh = shoff + shstrndx * shsz;
        strtabOff = is64 ? static_cast<std::size_t>(r64(sh + kShOffOff64))
                         : r32(sh + kShOffOff32);
    }

    for (uint16_t i = 0; i < shnum; ++i) {
        std::size_t sh = shoff + i * shsz;
        ElfSection sec;
        uint32_t nameOff = r32(sh + kShNameOff);
        sec.type = r32(sh + kShTypeOff);

        if (is64) {
            sec.flags   = static_cast<uint32_t>(r64(sh + kShFlagsOff));
            sec.addr    = r64(sh + kShAddrOff64);
            sec.offset  = r64(sh + kShOffOff64);
            sec.size    = r64(sh + kShSizeOff64);
            sec.link    = r32(sh + kShLinkOff64);
            sec.info    = r32(sh + kShInfoOff64);
            sec.entsize = r64(sh + kShEntSz64);
        } else {
            sec.flags   = r32(sh + kShFlagsOff);
            sec.addr    = r32(sh + kShAddrOff32);
            sec.offset  = r32(sh + kShOffOff32);
            sec.size    = r32(sh + kShSizeOff32);
            sec.link    = r32(sh + kShLinkOff32);
            sec.info    = r32(sh + kShInfoOff32);
            sec.entsize = r32(sh + kShEntSz32);
        }

        if (strtabOff != 0) {
            sec.name = rStr(strtabOff + nameOff);
        }
        secs.push_back(sec);
    }
    return secs;
}

const LoaderSim::ElfSection*
LoaderSim::findElfSection(const std::vector<ElfSection>& secs,
                           const std::string& name) const
{
    for (const auto& s : secs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// ── ELF RELA/REL relocations ─────────────────────────────────────────────────

std::vector<RelocRecord> LoaderSim::applyELFRelocations(uint64_t newBase) const
{
    std::vector<RelocRecord> recs;
    auto elfSecs = parseElfSections();
    uint64_t delta = newBase - _imageBase;

    for (const auto& sec : elfSecs) {
        if (sec.type != kSHT_RELA && sec.type != kSHT_REL) continue;
        if (sec.entsize == 0) continue;
        // Filter: only .rela.dyn / .rel.dyn (not .plt)
        if (sec.name.find(".plt") != std::string::npos) continue;

        bool isRela = (sec.type == kSHT_RELA);
        std::size_t entSz  = static_cast<std::size_t>(sec.entsize);
        std::size_t numEnt = entSz > 0 ? static_cast<std::size_t>(sec.size / entSz) : 0;

        for (std::size_t i = 0; i < numEnt; ++i) {
            std::size_t eOff = static_cast<std::size_t>(sec.offset) + i * entSz;
            if (!inBounds(eOff, entSz)) break;

            uint64_t offset  = rPtr(eOff);
            uint64_t info    = rPtr(eOff + (_is64Bit ? 8u : 4u));
            int64_t  addend  = isRela ? static_cast<int64_t>(r64(eOff + 16)) : 0;

            uint32_t rtype   = _is64Bit ? static_cast<uint32_t>(info & 0xFFFFFFFF)
                                        : static_cast<uint32_t>(info & 0xFF);

            RelocRecord rec;
            rec.vma    = offset + delta;
            rec.addend = static_cast<int32_t>(addend);
            rec.type   = static_cast<uint8_t>(rtype);

            // R_X86_64_RELATIVE (8) / R_386_RELATIVE (8): no symbol, just delta.
            if (rtype == 8) {
                recs.push_back(rec);
                continue;
            }
            // R_X86_64_64 (1), R_X86_64_GLOB_DAT (6), R_X86_64_JUMP_SLOT (7)
            if (rtype == 1 || rtype == 6 || rtype == 7) {
                recs.push_back(rec);
            }
        }
    }
    return recs;
}

// ── ELF PLT import resolution ─────────────────────────────────────────────────

std::vector<ImportRef> LoaderSim::resolveELFImports() const
{
    std::vector<ImportRef> imports;
    auto elfSecs = parseElfSections();

    // Find .dynsym and .dynstr sections.
    const ElfSection* dynsym = findElfSection(elfSecs, ".dynsym");
    const ElfSection* dynstr = findElfSection(elfSecs, ".dynstr");
    const ElfSection* plt    = findElfSection(elfSecs, ".plt");

    if (!dynsym || !dynstr) return imports;

    // Find .rela.plt (.rel.plt on 32-bit).
    const ElfSection* relaPlt = findElfSection(elfSecs, ".rela.plt");
    if (!relaPlt) relaPlt = findElfSection(elfSecs, ".rel.plt");
    if (!relaPlt) return imports;

    bool isRela   = (relaPlt->type == kSHT_RELA);
    std::size_t entSz = static_cast<std::size_t>(relaPlt->entsize);
    if (entSz == 0) entSz = isRela ? (_is64Bit ? 24u : 12u) : (_is64Bit ? 16u : 8u);
    std::size_t numEnt = static_cast<std::size_t>(relaPlt->size / entSz);

    // Symbol entry size.
    std::size_t symSz = _is64Bit ? 24u : 16u;

    // PLT stub layout: entry 0 is the resolver (16 bytes), each subsequent
    // stub is 16 bytes.  So stub N (0-based) is at plt->addr + 16 + 16*N.
    uint64_t pltBase = plt ? plt->addr : 0;

    for (std::size_t i = 0; i < numEnt; ++i) {
        std::size_t eOff = static_cast<std::size_t>(relaPlt->offset) + i * entSz;
        if (!inBounds(eOff, entSz)) break;

        uint64_t info = rPtr(eOff + (_is64Bit ? 8u : 4u));
        uint32_t symIdx = _is64Bit ? static_cast<uint32_t>(info >> 32)
                                   : static_cast<uint32_t>(info >> 8);

        // Read symbol name from .dynsym + .dynstr.
        std::size_t symOff = static_cast<std::size_t>(dynsym->offset) + symIdx * symSz;
        std::string symName;
        if (inBounds(symOff, symSz)) {
            uint32_t nameIdx = r32(symOff); // st_name is always first field
            symName = rStr(static_cast<std::size_t>(dynstr->offset) + nameIdx);
        }

        ImportRef imp;
        imp.symbol = symName;
        imp.dll    = "";  // ELF doesn't have per-import DLL names in .rela.plt

        // PLT stub address.
        if (plt) {
            imp.vma = pltBase + 16 + i * 16;
        } else {
            // Fall back to GOT entry address from the reloc offset.
            imp.vma = rPtr(eOff);
        }
        imports.push_back(imp);
    }
    return imports;
}

// ── ELF .init_array ───────────────────────────────────────────────────────────

std::vector<TLSCallback> LoaderSim::parseELFInitArray() const
{
    std::vector<TLSCallback> cbs;
    auto elfSecs = parseElfSections();
    std::size_t ptrSz = _is64Bit ? 8u : 4u;
    uint32_t idx = 0;

    for (const char* secName : {".init_array", ".preinit_array"}) {
        const ElfSection* sec = findElfSection(elfSecs, secName);
        if (!sec || sec->size == 0) continue;

        std::size_t count = static_cast<std::size_t>(sec->size / ptrSz);
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t off = static_cast<std::size_t>(sec->offset) + i * ptrSz;
            if (!inBounds(off, ptrSz)) break;
            uint64_t fnVA = _is64Bit ? r64(off) : r32(off);
            if (fnVA == 0 || fnVA == ~0ULL) continue;
            TLSCallback cb;
            cb.vma           = fnVA;
            cb.syntheticName = "__tls_callback_" + std::to_string(idx++);
            cbs.push_back(cb);
        }
    }
    return cbs;
}

// ── ELF section list to SectionDesc ──────────────────────────────────────────

std::vector<SectionDesc> LoaderSim::parseSections() const
{
    std::vector<SectionDesc> result;
    if (_isELF) {
        for (const auto& s : parseElfSections()) {
            if (s.size == 0 || s.offset == 0) continue;
            SectionDesc d;
            d.name      = s.name;
            d.vma       = s.addr;
            d.rawOffset = s.offset;
            d.rawSize   = static_cast<uint64_t>(s.size);
            d.virtSize  = static_cast<uint64_t>(s.size);
            d.executable = (s.flags & 0x4) != 0;
            d.writable   = (s.flags & 0x1) != 0;
            result.push_back(d);
        }
    } else {
        for (const auto& s : parsePESections()) {
            SectionDesc d;
            d.name      = s.name;
            d.vma       = s.vma;
            d.rawOffset = s.rawOff;
            d.rawSize   = s.rawSize;
            d.virtSize  = std::max(s.virtSize, s.rawSize);
            d.executable= (s.chars & 0x20000000) != 0;
            d.writable  = (s.chars & 0x80000000) != 0;
            d.readable  = (s.chars & 0x40000000) != 0;
            result.push_back(d);
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// load() — full pipeline
// ═══════════════════════════════════════════════════════════════════════════════

LoadedImage LoaderSim::load()
{
    LoadedImage img;
    img.imageBase = _imageBase;
    img.loadBias  = 0; // we don't randomly rebase; just represent preferred base
    img.sections  = parseSections();

    // ── 1. Imports & non-decompilable regions ────────────────────────────────
    std::vector<ImportRef> rawImports;
    if (_isELF) {
        rawImports = resolveELFImports();
        // Mark PLT section non-decompilable.
        auto elfSecs = parseElfSections();
        const auto* plt = findElfSection(elfSecs, ".plt");
        if (plt && plt->size > 0) {
            NonDecompilableRegion nd;
            nd.start  = plt->addr;
            nd.end    = plt->addr + plt->size;
            nd.reason = "PLT stub";
            img.nonDecompilable.push_back(nd);
        }
    } else {
        rawImports = resolvePEImports();

        // Mark IAT entries non-decompilable (each 4/8-byte slot).
        for (const auto& imp : rawImports) {
            NonDecompilableRegion nd;
            nd.start  = imp.vma;
            nd.end    = imp.vma + (_is64Bit ? 8u : 4u);
            nd.reason = "IAT thunk";
            img.nonDecompilable.push_back(nd);
        }

        // Delay-load imports.
        auto delayImps = resolvePEDelayImports();
        rawImports.insert(rawImports.end(), delayImps.begin(), delayImps.end());
    }

    // Build importByVma index.
    for (std::size_t i = 0; i < rawImports.size(); ++i) {
        img.importByVma[rawImports[i].vma] = i;
    }
    img.imports = std::move(rawImports);

    // ── 2. TLS / init callbacks ──────────────────────────────────────────────
    if (_isELF) {
        img.tlsCallbacks = parseELFInitArray();
    } else {
        img.tlsCallbacks = parsePETLS();
    }

    // ── 3. Base relocations ──────────────────────────────────────────────────
    if (_isELF) {
        img.relocations = applyELFRelocations(_imageBase);
    } else {
        img.relocations = applyPERelocations(_imageBase);
    }

    // ── 4. Entry point ───────────────────────────────────────────────────────
    if (!_isELF && _size >= 0x40) {
        std::size_t ntOff  = peNtOffset(_data, _size);
        if (ntOff > 0) {
            std::size_t optOff = ntOff + 4 + 20;
            uint16_t magic = r16(optOff + kMagic);
            uint64_t ib = (magic == kOptMagicPE32P)
                ? r64(optOff + kImageBase64)
                : r32(optOff + kImageBase32);
            uint32_t epRva = r32(optOff + kEntryRVA64); // same offset for 32+64
            img.entryPoint = ib + epRva;
        }
    } else if (_isELF && _size >= 64) {
        // e_entry: offset 24 (32-bit) or offset 24 (64-bit) in ELF header.
        img.entryPoint = _is64Bit ? r64(24) : r32(24);
    }

    return img;
}

} // namespace loader_sim
} // namespace retdec
