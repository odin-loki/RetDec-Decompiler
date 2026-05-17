/**
 * @file src/fileformat/lattice/format_lattice.cpp
 * @brief Signature-Lattice file format parser — full implementation.
 */

#include "retdec/fileformat/lattice/format_lattice.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <future>
#include <numeric>
#include <string>
#include <vector>

namespace retdec {
namespace fileformat {
namespace lattice {

// ─── Byte helpers ────────────────────────────────────────────────────────────

static inline uint8_t  u8 (const uint8_t *p) { return p[0]; }
static inline uint16_t u16le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static inline uint16_t u16be(const uint8_t *p) {
    return static_cast<uint16_t>(p[1]) | (static_cast<uint16_t>(p[0]) << 8);
}
static inline uint32_t u32le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0])       | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static inline uint32_t u32be(const uint8_t *p) {
    return static_cast<uint32_t>(p[3])       | (static_cast<uint32_t>(p[2]) << 8) |
           (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[0]) << 24);
}
static inline uint64_t u64le(const uint8_t *p) {
    return static_cast<uint64_t>(u32le(p)) | (static_cast<uint64_t>(u32le(p+4)) << 32);
}
static inline uint64_t u64be(const uint8_t *p) {
    return static_cast<uint64_t>(u32be(p+4)) | (static_cast<uint64_t>(u32be(p)) << 32);
}

static bool inBounds(size_t off, size_t len, size_t fileSize) {
    return off < fileSize && len <= fileSize - off;
}

// ─── Plausibility checks ─────────────────────────────────────────────────────

static constexpr uint32_t kMaxSaneSecCount = 9999;
static constexpr uint32_t kMinLoadAddr     = 0x1000;
static constexpr uint64_t kMaxLoadAddr64   = 0xFFFF'FFFF'FFFF'0000ULL;

// ─── ELF helpers ─────────────────────────────────────────────────────────────

// ELF class
static constexpr uint8_t ELFCLASS32 = 1;
static constexpr uint8_t ELFCLASS64 = 2;
// ELF data encoding
static constexpr uint8_t ELFDATA2LSB = 1;
static constexpr uint8_t ELFDATA2MSB = 2;
// e_type
static constexpr uint16_t ET_EXEC = 2;
static constexpr uint16_t ET_DYN  = 3;
// ELF segment types
static constexpr uint32_t PT_LOAD   = 1;
static constexpr uint32_t PT_TLS    = 7;
// ELF section types
static constexpr uint32_t SHT_DYNSYM  = 11;
static constexpr uint32_t SHT_STRTAB  = 3;
static constexpr uint32_t SHT_RELA    = 4;
static constexpr uint32_t SHT_REL     = 9;
// ELF reloc types (x86-64, simplified)
static constexpr uint32_t R_X86_64_JUMP_SLOT = 7;
static constexpr uint32_t R_386_JUMP_SLOT    = 7;
static constexpr uint32_t R_ARM_JUMP_SLOT    = 22;
static constexpr uint32_t R_AARCH64_JUMP_SLOT = 1026;
// ELF section flags
static constexpr uint32_t SHF_EXECINSTR = 4;
static constexpr uint32_t SHF_WRITE     = 1;
static constexpr uint32_t SHF_ALLOC     = 2;

// e_machine → Arch
static Arch elfMachineToArch(uint16_t machine) {
    switch (machine) {
        case 0x03: return Arch::X86;
        case 0x3E: return Arch::X86_64;
        case 0x28: return Arch::ARM;
        case 0xB7: return Arch::AArch64;
        case 0x08: return Arch::MIPS;
        case 0x14: return Arch::PowerPC;
        case 0x15: return Arch::PowerPC64;
        case 0xF3: return Arch::RISC_V;
        default:   return Arch::Unknown;
    }
}

// Safe null-terminated string from a section data byte range
static std::string safeStr(const uint8_t *base, size_t tableSize, size_t off) {
    if (off >= tableSize) return {};
    size_t maxLen = tableSize - off;
    size_t len = strnlen(reinterpret_cast<const char *>(base + off), maxLen);
    return std::string(reinterpret_cast<const char *>(base + off), len);
}

static FormatResult parseELF(const uint8_t *data, size_t size,
                              bool is64, bool isLE, const std::string &name)
{
    FormatResult res;
    res.name       = name;
    res.format     = is64 ? DetectedFormat::ELF64 : DetectedFormat::ELF32;
    res.endianness = isLE ? Endianness::Little : Endianness::Big;
    res.addressSize = is64 ? 64 : 32;

    auto u16 = isLE ? u16le : u16be;
    auto u32 = isLE ? u32le : u32be;
    auto u64 = isLE ? u64le : u64be;

    // ── ELF header ──────────────────────────────────────────────────────────
    // Minimum size: 52 bytes for ELF32 (0x34), 64 bytes for ELF64 (0x40).
    const size_t minHdr = is64 ? 64u : 52u;
    if (size < minHdr) { res.format = DetectedFormat::Unknown; return res; }

    uint16_t e_type    = u16(data + 16);
    uint16_t e_machine = u16(data + 18);
    res.architecture   = elfMachineToArch(e_machine);

    uint64_t e_entry, e_phoff, e_shoff;
    uint16_t e_phnum, e_phentsize, e_shnum, e_shentsize, e_shstrndx;

    if (!is64) {
        if (size < 52) { res.corruption.sectionOffsetsInvalid = true; return res; }
        e_entry    = u32(data + 24);
        e_phoff    = u32(data + 28);
        e_shoff    = u32(data + 32);
        e_phentsize = u16(data + 42);
        e_phnum    = u16(data + 44);
        e_shentsize = u16(data + 46);
        e_shnum    = u16(data + 48);
        e_shstrndx = u16(data + 50);
    } else {
        e_entry    = u64(data + 24);
        e_phoff    = u64(data + 32);
        e_shoff    = u64(data + 40);
        e_phentsize = u16(data + 54);
        e_phnum    = u16(data + 56);
        e_shentsize = u16(data + 58);
        e_shnum    = u16(data + 60);
        e_shstrndx = u16(data + 62);
    }

    res.entryPoint = e_entry;

    // Plausibility: section count
    if (e_shnum > kMaxSaneSecCount) {
        res.corruption.sectionCountSuspicious = true;
        e_shnum = 0; // skip section parsing for absurd counts
    }

    // ── Determine load bias from PT_LOAD ─────────────────────────────────────
    uint64_t minLoadVA   = UINT64_MAX;
    uint64_t imageBase   = 0;

    if (e_phoff != 0 && e_phnum > 0 && e_phentsize >= (is64 ? 56u : 32u)) {
        for (uint16_t pi = 0; pi < e_phnum; ++pi) {
            size_t poff = static_cast<size_t>(e_phoff) + pi * e_phentsize;
            if (!inBounds(poff, e_phentsize, size)) break;
            uint32_t p_type = u32(data + poff);
            if (p_type == PT_LOAD) {
                uint64_t p_vaddr = is64 ? u64(data + poff + 16) : u32(data + poff + 8);
                if (p_vaddr < minLoadVA) minLoadVA = p_vaddr;
            }
            if (p_type == PT_TLS && !res.tls) {
                TLSInfo tls;
                tls.startAddressOfRawData = is64 ? u64(data+poff+8)  : u32(data+poff+4);
                tls.endAddressOfRawData   = is64 ? u64(data+poff+16) : u32(data+poff+8);
                res.tls = tls;
            }
        }
        if (minLoadVA != UINT64_MAX)
            imageBase = minLoadVA;
    }
    res.imageBase = imageBase;
    res.loadBias  = 0; // PIE: actual load addr - imageBase (unknown at parse time)

    // Plausibility: entry point in [minLoadVA, maxLoadVA]
    if (e_type == ET_EXEC && e_entry != 0) {
        uint64_t maxLA = is64 ? kMaxLoadAddr64 : 0xFFFF'FFFFull;
        if (e_entry < kMinLoadAddr || e_entry > maxLA) {
            res.corruption.entryPointOutOfRange = true;
        }
    }

    // ── Section headers ──────────────────────────────────────────────────────
    // Find section string table first
    const uint8_t *shstr_data = nullptr;
    size_t         shstr_size = 0;

    if (e_shstrndx != 0 && e_shstrndx < e_shnum && e_shentsize > 0) {
        size_t sidx_off = static_cast<size_t>(e_shoff) + e_shstrndx * e_shentsize;
        if (inBounds(sidx_off, e_shentsize, size)) {
            uint64_t sh_off  = is64 ? u64(data + sidx_off + 24) : u32(data + sidx_off + 16);
            uint64_t sh_size = is64 ? u64(data + sidx_off + 32) : u32(data + sidx_off + 20);
            if (sh_off + sh_size <= size) {
                shstr_data = data + sh_off;
                shstr_size = static_cast<size_t>(sh_size);
            } else {
                res.corruption.strTableOffsetInvalid = true;
            }
        }
    }

    // Indices for dynsym and dynstr (needed for imports)
    uint32_t dynsym_idx = UINT32_MAX, dynstr_idx = UINT32_MAX;
    // Also find .rela.plt or .rel.plt
    uint32_t relaplt_idx = UINT32_MAX;
    bool     hasRela = false;

    for (uint16_t si = 0; si < e_shnum; ++si) {
        size_t soff = static_cast<size_t>(e_shoff) + si * e_shentsize;
        if (!inBounds(soff, e_shentsize, size)) break;

        uint32_t sh_name  = u32(data + soff);
        uint32_t sh_type  = u32(data + soff + 4);
        uint64_t sh_flags = is64 ? u64(data + soff + 8) : u32(data + soff + 8);
        uint64_t sh_addr  = is64 ? u64(data + soff + 16) : u32(data + soff + 12);
        uint64_t sh_off2  = is64 ? u64(data + soff + 24) : u32(data + soff + 16);
        uint64_t sh_size2 = is64 ? u64(data + soff + 32) : u32(data + soff + 20);

        std::string secName;
        if (shstr_data)
            secName = safeStr(shstr_data, shstr_size, sh_name);

        SectionInfo sec;
        sec.name           = secName;
        sec.virtualAddress = sh_addr;
        sec.virtualSize    = sh_size2;
        sec.fileOffset     = sh_off2;
        sec.fileSize       = sh_size2;
        sec.characteristics = static_cast<uint32_t>(sh_flags);
        sec.isExecutable   = (sh_flags & SHF_EXECINSTR) != 0;
        sec.isWritable     = (sh_flags & SHF_WRITE)     != 0;
        sec.isReadable     = (sh_flags & SHF_ALLOC)     != 0;
        if (si > 0) res.sections.push_back(sec);

        if (sh_type == SHT_DYNSYM) dynsym_idx = si;
        if (sh_type == SHT_STRTAB && secName == ".dynstr") dynstr_idx = si;
        if ((sh_type == SHT_RELA || sh_type == SHT_REL) &&
            (secName == ".rela.plt" || secName == ".rel.plt")) {
            relaplt_idx = si;
            hasRela = (sh_type == SHT_RELA);
        }
    }

    // ── Imports via .rela.plt / .rel.plt + .dynsym + .dynstr ────────────────
    if (relaplt_idx != UINT32_MAX && dynsym_idx != UINT32_MAX && dynstr_idx != UINT32_MAX
        && e_shentsize > 0)
    {
        // Get section info
        auto getSecOff  = [&](uint32_t idx) -> uint64_t {
            size_t o = static_cast<size_t>(e_shoff) + idx * e_shentsize;
            return is64 ? u64(data + o + 24) : u32(data + o + 16);
        };
        auto getSecSize = [&](uint32_t idx) -> uint64_t {
            size_t o = static_cast<size_t>(e_shoff) + idx * e_shentsize;
            return is64 ? u64(data + o + 32) : u32(data + o + 20);
        };
        auto getSecLink = [&](uint32_t idx) -> uint32_t {
            size_t o = static_cast<size_t>(e_shoff) + idx * e_shentsize;
            return u32(data + o + (is64 ? 40 : 24));
        };

        uint64_t rela_off  = getSecOff(relaplt_idx);
        uint64_t rela_sz   = getSecSize(relaplt_idx);
        uint32_t sym_link  = getSecLink(relaplt_idx); // points to .dynsym
        (void)sym_link;

        uint64_t sym_off   = getSecOff(dynsym_idx);
        uint64_t sym_sz    = getSecSize(dynsym_idx);
        uint64_t str_off   = getSecOff(dynstr_idx);
        uint64_t str_sz    = getSecSize(dynstr_idx);

        // Sizes of rela/sym entries
        size_t rela_ent = hasRela ? (is64 ? 24u : 12u) : (is64 ? 16u : 8u);
        size_t sym_ent  = is64 ? 24u : 16u;

        if (inBounds(static_cast<size_t>(rela_off), static_cast<size_t>(rela_sz), size) &&
            inBounds(static_cast<size_t>(sym_off),  static_cast<size_t>(sym_sz),  size) &&
            inBounds(static_cast<size_t>(str_off),  static_cast<size_t>(str_sz),  size) &&
            rela_ent > 0 && sym_ent > 0)
        {
            const uint8_t *rela_base = data + rela_off;
            const uint8_t *sym_base  = data + sym_off;
            const uint8_t *str_base  = data + str_off;
            size_t num_rela = static_cast<size_t>(rela_sz) / rela_ent;
            size_t num_sym  = static_cast<size_t>(sym_sz)  / sym_ent;

            for (size_t ri = 0; ri < num_rela; ++ri) {
                const uint8_t *re = rela_base + ri * rela_ent;
                uint64_t r_offset = is64 ? u64(re) : u32(re);
                uint32_t r_info32 = u32(re + (is64 ? 8 : 4));
                uint32_t sym_idx  = is64 ? static_cast<uint32_t>(u64(re + 8) >> 32) : (r_info32 >> 8);

                if (sym_idx == 0 || sym_idx >= num_sym) continue;
                const uint8_t *sym = sym_base + sym_idx * sym_ent;
                uint32_t st_name  = u32(sym);
                std::string fn    = safeStr(str_base, static_cast<size_t>(str_sz), st_name);
                if (fn.empty()) continue;

                ImportEntry imp;
                imp.functionName = fn;
                imp.address      = r_offset;
                res.imports.push_back(imp);
            }
        } else {
            res.corruption.importDirectoryInvalid = true;
        }
    }

    // ── Exports from .dynsym (GLOBAL/WEAK, non-UNDEF) ─────────────────────
    if (dynsym_idx != UINT32_MAX && dynstr_idx != UINT32_MAX && e_shentsize > 0) {
        size_t sym_off  = static_cast<size_t>(
            is64 ? u64(data + static_cast<size_t>(e_shoff) + dynsym_idx * e_shentsize + 24)
                 : u32(data + static_cast<size_t>(e_shoff) + dynsym_idx * e_shentsize + 16));
        size_t sym_sz   = static_cast<size_t>(
            is64 ? u64(data + static_cast<size_t>(e_shoff) + dynsym_idx * e_shentsize + 32)
                 : u32(data + static_cast<size_t>(e_shoff) + dynsym_idx * e_shentsize + 20));
        size_t str_off  = static_cast<size_t>(
            is64 ? u64(data + static_cast<size_t>(e_shoff) + dynstr_idx * e_shentsize + 24)
                 : u32(data + static_cast<size_t>(e_shoff) + dynstr_idx * e_shentsize + 16));
        size_t str_sz   = static_cast<size_t>(
            is64 ? u64(data + static_cast<size_t>(e_shoff) + dynstr_idx * e_shentsize + 32)
                 : u32(data + static_cast<size_t>(e_shoff) + dynstr_idx * e_shentsize + 20));

        size_t sym_ent = is64 ? 24u : 16u;
        if (inBounds(sym_off, sym_sz, size) && inBounds(str_off, str_sz, size) && sym_ent > 0) {
            const uint8_t *sym_base = data + sym_off;
            const uint8_t *str_base = data + str_off;
            size_t num_sym = sym_sz / sym_ent;
            for (size_t si = 1; si < num_sym; ++si) {
                const uint8_t *sym = sym_base + si * sym_ent;
                uint32_t st_name  = u32(sym);
                uint8_t  st_info  = is64 ? sym[4] : sym[12];
                uint8_t  st_bind  = st_info >> 4;
                uint8_t  st_shndx_lo = is64 ? sym[6] : sym[14]; // low byte of st_shndx
                // bind: 1=GLOBAL, 2=WEAK; shndx != 0 (UNDEF) and != 0xFFFF (ABS)
                if ((st_bind == 1 || st_bind == 2) && st_shndx_lo != 0) {
                    uint64_t st_value = is64 ? u64(sym + 8) : u32(sym + 4);
                    std::string fn = safeStr(str_base, str_sz, st_name);
                    if (!fn.empty() && st_value != 0) {
                        ExportEntry exp;
                        exp.functionName = fn;
                        exp.address      = st_value;
                        res.exports.push_back(exp);
                    }
                }
            }
        }
    }

    return res;
}

// ─── PE helpers ──────────────────────────────────────────────────────────────

static Arch peMachineToArch(uint16_t machine) {
    switch (machine) {
        case 0x014C: return Arch::X86;
        case 0x8664: return Arch::X86_64;
        case 0x01C0: return Arch::ARM;
        case 0xAA64: return Arch::AArch64;
        case 0x0200: return Arch::Unknown; // IA-64 (legacy)
        default:     return Arch::Unknown;
    }
}

static FormatResult parsePE(const uint8_t *data, size_t size, const std::string &name)
{
    FormatResult res;
    res.name       = name;
    res.endianness = Endianness::Little;

    if (size < 64) { res.format = DetectedFormat::Unknown; return res; }

    // DOS header: e_lfanew at offset 0x3C
    uint32_t e_lfanew = u32le(data + 0x3C);

    // Plausibility: e_lfanew should be sane
    if (e_lfanew < 0x40 || e_lfanew >= size || size - e_lfanew < 24) {
        // Malformed: try heuristic scan for "PE\0\0"
        bool found = false;
        for (size_t off = 0x40; off + 4 < std::min(size, (size_t)0x1000); ++off) {
            if (data[off] == 'P' && data[off+1] == 'E' && data[off+2] == 0 && data[off+3] == 0) {
                e_lfanew = static_cast<uint32_t>(off);
                found = true;
                break;
            }
        }
        if (!found) { res.format = DetectedFormat::Unknown; return res; }
    }

    const uint8_t *pe = data + e_lfanew;
    if (size - e_lfanew < 24) { res.format = DetectedFormat::Unknown; return res; }

    // "PE\0\0" signature
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) {
        res.format = DetectedFormat::Unknown; return res;
    }

    // COFF file header
    uint16_t machine      = u16le(pe + 4);
    uint16_t num_sections = u16le(pe + 6);
    uint16_t opt_size     = u16le(pe + 20);

    res.architecture = peMachineToArch(machine);

    if (num_sections > kMaxSaneSecCount) {
        res.corruption.sectionCountSuspicious = true;
        num_sections = 0;
    }

    // Optional header
    const uint8_t *opt = pe + 24;
    if (size < e_lfanew + 24 + opt_size) {
        res.format = DetectedFormat::Unknown; return res;
    }
    if (opt_size < 2) { res.format = DetectedFormat::Unknown; return res; }

    uint16_t magic = u16le(opt);
    bool is64 = (magic == 0x020B);
    bool is32 = (magic == 0x010B);
    if (!is64 && !is32) { res.format = DetectedFormat::Unknown; return res; }

    res.format     = is64 ? DetectedFormat::PE64 : DetectedFormat::PE32;
    res.addressSize = is64 ? 64 : 32;

    uint64_t image_base   = is64 ? u64le(opt + 24) : u32le(opt + 28);
    uint32_t ep_rva       = u32le(opt + 16);
    res.imageBase  = image_base;
    res.entryPoint = image_base + ep_rva;
    res.loadBias   = 0;

    // Data directory count
    uint32_t num_dd = is64 ? u32le(opt + 92) : u32le(opt + 92);
    size_t   dd_off = is64 ? 112u : 96u;

    // Plausibility: entry point
    if (ep_rva != 0) {
        uint32_t size_of_image = u32le(opt + (is64 ? 56 : 56));
        if (ep_rva >= size_of_image) {
            res.corruption.entryPointOutOfRange = true;
        }
    }

    // Section table: immediately after optional header
    size_t sec_tbl_off = e_lfanew + 24 + opt_size;
    for (uint16_t si = 0; si < num_sections; ++si) {
        size_t soff = sec_tbl_off + si * 40;
        if (!inBounds(soff, 40, size)) { res.corruption.sectionOffsetsInvalid = true; break; }
        const uint8_t *s = data + soff;
        char nameBuf[9] = {};
        std::memcpy(nameBuf, s, 8);
        SectionInfo sec;
        sec.name           = std::string(nameBuf);
        sec.virtualSize    = u32le(s + 8);
        sec.virtualAddress = image_base + u32le(s + 12);
        sec.fileSize       = u32le(s + 16);
        sec.fileOffset     = u32le(s + 20);
        uint32_t chars     = u32le(s + 36);
        sec.characteristics = chars;
        sec.isExecutable   = (chars & 0x20000000) != 0;
        sec.isWritable     = (chars & 0x80000000) != 0;
        sec.isReadable     = (chars & 0x40000000) != 0;
        res.sections.push_back(sec);
    }

    // Helper: resolve a data directory entry
    auto resolveDD = [&](size_t ddIdx) -> std::pair<uint64_t, uint32_t> {
        if (ddIdx >= num_dd) return {0,0};
        size_t off = e_lfanew + 24 + dd_off + ddIdx * 8;
        if (!inBounds(off, 8, size)) return {0,0};
        uint32_t rva = u32le(data + off);
        uint32_t sz  = u32le(data + off + 4);
        if (rva == 0) return {0,0};
        // Translate RVA to file offset
        for (auto &sec : res.sections) {
            uint64_t sec_rva_start = sec.virtualAddress - image_base;
            uint64_t sec_rva_end   = sec_rva_start + sec.virtualSize;
            if (rva >= sec_rva_start && rva < sec_rva_end) {
                uint64_t file_off = sec.fileOffset + (rva - sec_rva_start);
                return {file_off, sz};
            }
        }
        return {0,0};
    };

    // ── Import table (DD[1]) ─────────────────────────────────────────────────
    auto [imp_off, imp_sz] = resolveDD(1);
    if (imp_off != 0 && inBounds(static_cast<size_t>(imp_off), 20, size)) {
        const uint8_t *imp_desc = data + imp_off;
        for (size_t di = 0; ; di += 20) {
            if (!inBounds(static_cast<size_t>(imp_off) + di, 20, size)) break;
            const uint8_t *desc = imp_desc + di;
            uint32_t iat_rva  = u32le(desc);
            uint32_t name_rva = u32le(desc + 12);
            uint32_t iat_addr = u32le(desc + 16);
            if (iat_rva == 0 && name_rva == 0 && iat_addr == 0) break;

            // Resolve DLL name
            std::string dllName;
            auto [dll_off, dll_sz] = resolveDD(0); // unused; manual RVA resolve
            for (auto &sec : res.sections) {
                uint64_t sec_rva_start = sec.virtualAddress - image_base;
                if (name_rva >= sec_rva_start &&
                    name_rva < sec_rva_start + sec.virtualSize)
                {
                    size_t fo = static_cast<size_t>(sec.fileOffset + (name_rva - sec_rva_start));
                    if (fo < size) {
                        dllName = safeStr(data, size, fo);
                    }
                    break;
                }
            }

            // Walk IAT
            uint32_t thunk_rva = (iat_rva != 0) ? iat_rva : iat_addr;
            for (auto &sec : res.sections) {
                uint64_t sec_rva_start = sec.virtualAddress - image_base;
                if (thunk_rva >= sec_rva_start &&
                    thunk_rva < sec_rva_start + sec.virtualSize)
                {
                    size_t thunk_fo = static_cast<size_t>(sec.fileOffset + (thunk_rva - sec_rva_start));
                    size_t entry_sz = is64 ? 8u : 4u;
                    for (size_t ti = 0; ; ti += entry_sz) {
                        if (!inBounds(thunk_fo + ti, entry_sz, size)) break;
                        uint64_t thunk = is64 ? u64le(data + thunk_fo + ti)
                                               : u32le(data + thunk_fo + ti);
                        if (thunk == 0) break;
                        bool byOrd = is64 ? ((thunk >> 63) & 1) != 0 : ((thunk >> 31) & 1) != 0;
                        ImportEntry imp2;
                        imp2.moduleName = dllName;
                        if (byOrd) {
                            imp2.byOrdinal = true;
                            imp2.ordinal   = static_cast<uint16_t>(thunk & 0xFFFF);
                        } else {
                            uint32_t hint_rva = static_cast<uint32_t>(
                                is64 ? (thunk & 0x7FFF'FFFF'FFFF'FFFFULL) : (thunk & 0x7FFFFFFF));
                            for (auto &sec2 : res.sections) {
                                uint64_t s2 = sec2.virtualAddress - image_base;
                                if (hint_rva >= s2 && hint_rva < s2 + sec2.virtualSize) {
                                    size_t fo2 = static_cast<size_t>(sec2.fileOffset + (hint_rva - s2));
                                    if (fo2 + 2 < size)
                                        imp2.functionName = safeStr(data, size, fo2 + 2);
                                    break;
                                }
                            }
                        }
                        imp2.address = image_base + thunk_rva + ti;
                        res.imports.push_back(imp2);
                    }
                    break;
                }
            }
        }
    } else if (imp_off == 0 && imp_sz != 0) {
        res.corruption.importDirectoryInvalid = true;
    }

    // ── Export table (DD[0]) ─────────────────────────────────────────────────
    auto [exp_off, exp_sz] = resolveDD(0);
    if (exp_off != 0 && inBounds(static_cast<size_t>(exp_off), 40, size)) {
        const uint8_t *edir = data + exp_off;
        uint32_t num_funcs  = u32le(edir + 20);
        uint32_t num_names  = u32le(edir + 24);
        uint32_t addr_rva   = u32le(edir + 28);
        uint32_t name_rva   = u32le(edir + 32);
        uint32_t ord_base   = u32le(edir + 16);

        if (num_funcs > 100000 || num_names > 100000) {
            res.corruption.exportDirectoryInvalid = true;
        } else {
            // Resolve addr table
            for (auto &sec : res.sections) {
                uint64_t sec_rva_start = sec.virtualAddress - image_base;
                if (addr_rva >= sec_rva_start &&
                    addr_rva < sec_rva_start + sec.virtualSize)
                {
                    size_t addr_fo = static_cast<size_t>(sec.fileOffset + (addr_rva - sec_rva_start));
                    size_t name_fo_base = 0;
                    size_t ord_fo_base  = 0;
                    // Find name and ordinal table file offsets
                    for (auto &sec2 : res.sections) {
                        uint64_t s2 = sec2.virtualAddress - image_base;
                        if (name_rva >= s2 && name_rva < s2 + sec2.virtualSize)
                            name_fo_base = static_cast<size_t>(sec2.fileOffset + (name_rva - s2));
                    }
                    uint32_t ord_rva = u32le(edir + 36);
                    for (auto &sec2 : res.sections) {
                        uint64_t s2 = sec2.virtualAddress - image_base;
                        if (ord_rva >= s2 && ord_rva < s2 + sec2.virtualSize)
                            ord_fo_base = static_cast<size_t>(sec2.fileOffset + (ord_rva - s2));
                    }
                    for (uint32_t ni = 0; ni < num_names; ++ni) {
                        if (!inBounds(name_fo_base + ni*4, 4, size)) break;
                        if (!inBounds(ord_fo_base  + ni*2, 2, size)) break;
                        uint32_t fn_rva  = u32le(data + name_fo_base + ni*4);
                        uint16_t ord_idx = u16le(data + ord_fo_base  + ni*2);
                        if (!inBounds(addr_fo + ord_idx*4, 4, size)) continue;
                        uint32_t fn_addr_rva = u32le(data + addr_fo + ord_idx*4);
                        std::string fn;
                        for (auto &sec2 : res.sections) {
                            uint64_t s2 = sec2.virtualAddress - image_base;
                            if (fn_rva >= s2 && fn_rva < s2 + sec2.virtualSize) {
                                size_t fo2 = static_cast<size_t>(sec2.fileOffset + (fn_rva - s2));
                                fn = safeStr(data, size, fo2);
                                break;
                            }
                        }
                        ExportEntry exp2;
                        exp2.functionName = fn;
                        exp2.ordinal      = static_cast<uint16_t>(ord_base + ord_idx);
                        exp2.address      = image_base + fn_addr_rva;
                        res.exports.push_back(exp2);
                    }
                    break;
                }
            }
        }
    }

    // ── TLS directory (DD[9]) ────────────────────────────────────────────────
    auto [tls_off, tls_sz] = resolveDD(9);
    if (tls_off != 0 && inBounds(static_cast<size_t>(tls_off), 24, size)) {
        TLSInfo tls;
        const uint8_t *td = data + tls_off;
        tls.startAddressOfRawData = is64 ? u64le(td)     : u32le(td);
        tls.endAddressOfRawData   = is64 ? u64le(td+8)  : u32le(td+4);
        tls.addressOfIndex        = is64 ? u64le(td+16) : u32le(td+8);
        uint64_t cb_rva           = is64 ? u64le(td+24) : u32le(td+12);
        // Walk callback array (VAs)
        if (cb_rva != 0) {
            for (auto &sec : res.sections) {
                uint64_t sva = sec.virtualAddress;
                if (cb_rva >= sva && cb_rva < sva + sec.virtualSize) {
                    size_t cfo = static_cast<size_t>(sec.fileOffset + (cb_rva - sva));
                    size_t stride = is64 ? 8u : 4u;
                    for (size_t ci = 0; ; ci += stride) {
                        if (!inBounds(cfo + ci, stride, size)) break;
                        uint64_t va = is64 ? u64le(data + cfo + ci) : u32le(data + cfo + ci);
                        if (va == 0) break;
                        tls.callbackAddresses.push_back(va);
                    }
                    break;
                }
            }
        }
        res.tls = tls;
    }

    return res;
}

// ─── Mach-O helpers ──────────────────────────────────────────────────────────

static Arch machoCPUTypeToArch(uint32_t cputype) {
    switch (cputype & ~0x01000000u) { // strip CPU_ARCH_ABI64 bit
        case 7:  return (cputype & 0x01000000) ? Arch::X86_64 : Arch::X86;
        case 12: return (cputype & 0x01000000) ? Arch::AArch64 : Arch::ARM;
        case 18: return Arch::PowerPC; // PowerPC
        case 24: return Arch::RISC_V;
        default: return Arch::Unknown;
    }
}

static FormatResult parseMachO(const uint8_t *data, size_t size, const std::string &name)
{
    FormatResult res;
    res.name = name;
    if (size < 28) { res.format = DetectedFormat::Unknown; return res; }

    uint32_t magic = u32le(data);
    bool is64  = (magic == 0xFEEDFACFu || magic == 0xCFFAEDFEu);
    bool isLE  = (magic == 0xFEEDFACEu || magic == 0xFEEDFACFu);
    if (!isLE && magic != 0xCEFAEDFEu && magic != 0xCFFAEDFEu) {
        res.format = DetectedFormat::Unknown; return res;
    }

    res.endianness = isLE ? Endianness::Little : Endianness::Big;
    res.format     = is64 ? DetectedFormat::MachO64 : DetectedFormat::MachO32;
    res.addressSize = is64 ? 64 : 32;

    auto u32 = isLE ? u32le : u32be;
    auto u64 = isLE ? u64le : u64be;

    uint32_t cputype    = u32(data + 4);
    uint32_t num_cmds   = u32(data + 16);
    uint32_t sz_cmds    = u32(data + 20);
    res.architecture    = machoCPUTypeToArch(cputype);

    size_t cmd_off = is64 ? 32u : 28u;
    if (cmd_off + sz_cmds > size) sz_cmds = static_cast<uint32_t>(size - cmd_off);

    for (uint32_t ci = 0; ci < num_cmds; ++ci) {
        if (!inBounds(cmd_off, 8, size)) break;
        uint32_t cmd   = u32(data + cmd_off);
        uint32_t cmdsize = u32(data + cmd_off + 4);
        if (cmdsize < 8) break;

        // LC_SEGMENT (0x1) or LC_SEGMENT_64 (0x19)
        if (cmd == 0x1 || cmd == 0x19) {
            bool s64 = (cmd == 0x19);
            size_t min_sz = s64 ? 64u : 56u;
            if (!inBounds(cmd_off, min_sz, size)) { cmd_off += cmdsize; continue; }
            char segname[17] = {};
            std::memcpy(segname, data + cmd_off + 8, 16);
            uint64_t vmaddr  = s64 ? u64(data + cmd_off + 24) : u32(data + cmd_off + 24);
            uint64_t vmsize  = s64 ? u64(data + cmd_off + 32) : u32(data + cmd_off + 28);
            uint64_t fileoff = s64 ? u64(data + cmd_off + 40) : u32(data + cmd_off + 32);
            uint64_t filesz  = s64 ? u64(data + cmd_off + 48) : u32(data + cmd_off + 36);
            uint32_t initprot = s64 ? u32(data + cmd_off + 56) : u32(data + cmd_off + 44);
            SectionInfo sec;
            sec.name           = std::string(segname);
            sec.virtualAddress = vmaddr;
            sec.virtualSize    = vmsize;
            sec.fileOffset     = fileoff;
            sec.fileSize       = filesz;
            sec.isExecutable   = (initprot & 4) != 0;
            sec.isWritable     = (initprot & 2) != 0;
            sec.isReadable     = (initprot & 1) != 0;
            res.sections.push_back(sec);
        }

        // LC_MAIN (0x80000028) — entry point
        if (cmd == 0x80000028u && cmdsize >= 16) {
            uint64_t ep_off = u64(data + cmd_off + 8);
            // ep_off is offset from __TEXT segment start — approximate with imageBase + offset
            res.entryPoint = res.imageBase + ep_off;
        }

        // LC_LOAD_DYLIB (0xC), LC_LOAD_WEAK_DYLIB (0x18), LC_REEXPORT_DYLIB (0x1F)
        if ((cmd == 0xCu || cmd == 0x18u || cmd == 0x1Fu) && cmdsize >= 24) {
            uint32_t name_off = u32(data + cmd_off + 8);
            if (name_off < cmdsize) {
                std::string lib = safeStr(data + cmd_off, cmdsize, name_off);
                // Add a placeholder import for the library
                ImportEntry imp;
                imp.moduleName   = lib;
                imp.functionName = "(dylib)";
                res.imports.push_back(imp);
            }
        }

        cmd_off += cmdsize;
    }

    return res;
}

// ─── AR archive parser ────────────────────────────────────────────────────────

static FormatResult parseAR(const uint8_t *data, size_t size, const std::string &name,
                             const FormatLattice &lattice)
{
    FormatResult res;
    res.name   = name;
    res.format = DetectedFormat::ARArchive;

    // Global header: "!<arch>\n" (8 bytes) or "!<thin>\n"
    size_t off = 8;

    // Optional symbol table member (name "/" or "//") — skip
    std::vector<std::future<FormatResult>> futures;

    while (off + 60 <= size) {
        // AR member header (60 bytes)
        const uint8_t *hdr = data + off;
        // Name: 16 bytes, padded with spaces
        char memberName[17] = {};
        std::memcpy(memberName, hdr, 16);
        // Trim trailing spaces
        for (int i = 15; i >= 0 && memberName[i] == ' '; --i) memberName[i] = 0;

        // Size: 10 ASCII decimal bytes at offset 48
        char sizeBuf[11] = {};
        std::memcpy(sizeBuf, hdr + 48, 10);
        size_t memberSize = static_cast<size_t>(std::strtoull(sizeBuf, nullptr, 10));

        // End-of-archive magic: "`\n" at offset 58
        if (hdr[58] != '`' || hdr[59] != '\n') break;

        size_t dataOff = off + 60;
        if (dataOff + memberSize > size) memberSize = size - dataOff;

        // Skip special members: "/", "//", "/SYM64/"
        bool isSpecial = (memberName[0] == '/' && (memberName[1] == 0 || memberName[1] == '/'));
        if (!isSpecial && memberSize > 0) {
            const uint8_t *mdata = data + dataOff;
            size_t msz = memberSize;
            std::string mname(memberName);
            // Dispatch each member in parallel
            futures.push_back(std::async(std::launch::async,
                [&lattice, mdata, msz, mname]() {
                    return lattice.classify(mdata, msz, mname);
                }
            ));
        }

        // Members are padded to even size
        off = dataOff + memberSize + (memberSize & 1);
    }

    res.arMembers.reserve(futures.size());
    for (auto &f : futures) {
        res.arMembers.push_back(f.get());
    }
    return res;
}

// ─── Polyglot scoring (0xCAFEBABE) ───────────────────────────────────────────

/**
 * Score self-consistent internal cross-references for CAFEBABE ambiguity.
 * second_word > 30 → Java .class file (class version > 30 means Java 2+).
 * second_word in [1..30] → Mach-O fat binary (nfat_arch is reasonable).
 */
static DetectedFormat scoreCafeBabe(const uint8_t *data, size_t size)
{
    if (size < 8) return DetectedFormat::Unknown;
    uint32_t second_word = u32be(data + 4);
    if (second_word > 30)
        return DetectedFormat::Unknown; // Java .class — not a supported binary
    // Mach-O fat: nfat_arch = second_word, each arch_header is 20 bytes
    // Plausibility: at least 1 arch, total size matches
    if (second_word == 0) return DetectedFormat::Unknown;
    if (size >= 8 + second_word * 20)
        return DetectedFormat::MachOFat;
    return DetectedFormat::Unknown;
}

// ─── Main classify() ─────────────────────────────────────────────────────────

FormatResult FormatLattice::classify(const uint8_t *data, size_t size,
                                      const std::string &name) const
{
    if (!data || size == 0) {
        FormatResult r; r.name = name; return r;
    }

    // ── Decision lattice over first 512 bytes ─────────────────────────────
    // Node 0: Intel HEX — first byte ':'
    if (data[0] == ':') {
        FormatResult r;
        r.name   = name;
        r.format = DetectedFormat::IntelHex;
        return r;
    }

    // Node 1: ELF magic
    if (size >= 4 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        bool is64 = (size > 4 && data[4] == ELFCLASS64);
        bool isLE = !(size > 5 && data[5] == ELFDATA2MSB);
        return parseELF(data, size, is64, isLE, name);
    }

    // Node 2: PE (MZ or ZM)
    if (size >= 2 && ((data[0] == 'M' && data[1] == 'Z') || (data[0] == 'Z' && data[1] == 'M'))) {
        return parsePE(data, size, name);
    }

    // Node 3: AR / thin archive
    if (size >= 8 && (std::memcmp(data, "!<arch>\n", 8) == 0 ||
                      std::memcmp(data, "!<thin>\n", 8) == 0)) {
        return parseAR(data, size, name, *this);
    }

    // Node 4: Mach-O 32/64 slice
    if (size >= 4) {
        uint32_t m = u32le(data);
        if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || // LE
            m == 0xCEFAEDFEu || m == 0xCFFAEDFEu)  { // BE
            return parseMachO(data, size, name);
        }
    }

    // Node 5: 0xCAFEBABE — polyglot (Java .class OR fat Mach-O)
    if (size >= 8 && data[0] == 0xCA && data[1] == 0xFE &&
        data[2] == 0xBA && data[3] == 0xBE)
    {
        DetectedFormat fmt = scoreCafeBabe(data, size);
        if (fmt == DetectedFormat::MachOFat) {
            // Fat Mach-O: header is big-endian regardless of contained slices.
            // Layout: magic(4) | nfat_arch(4) | fat_arch[nfat_arch](20 each)
            // fat_arch: cputype(4) | cpusubtype(4) | offset(4) | size(4) | align(4)
            FormatResult r;
            r.name   = name;
            r.format = DetectedFormat::MachOFat;

            uint32_t nfat = u32be(data + 4);
            if (size >= 8 + static_cast<size_t>(nfat) * 20) {
                for (uint32_t ai = 0; ai < nfat; ++ai) {
                    const uint8_t *ah = data + 8 + ai * 20;
                    uint32_t slice_off  = u32be(ah + 8);
                    uint32_t slice_size = u32be(ah + 12);
                    if (slice_off == 0 || slice_size == 0) continue;
                    if (!inBounds(slice_off, slice_size, size)) continue;
                    // Classify each thin slice and record it.
                    FormatResult slice = classify(data + slice_off, slice_size,
                                                  name + "[arch" + std::to_string(ai) + "]");
                    r.arMembers.push_back(std::move(slice));
                }
            }
            return r;
        }
        // Java or unknown — not a binary we decompile
        FormatResult r; r.name = name; return r;
    }

    // Node 6: COFF — check machine id at offset 0
    if (size >= 2) {
        uint16_t machine = u16le(data);
        // Known COFF machine IDs (x86, x64, ARM, AArch64, etc.)
        static const uint16_t coffMachines[] = {
            0x014C, 0x014D, 0x014E, 0x014F, 0x0160, 0x0162, 0x0166, 0x0169,
            0x0184, 0x01A2, 0x01A4, 0x01A6, 0x01A8, 0x01C0, 0x01C2, 0x01C4,
            0x01D3, 0x01F0, 0x0200, 0x0268, 0x0284, 0x0290, 0x0366, 0x0466,
            0x0520, 0x0EBC, 0x8664, 0x9041, 0xAA64, 0xC0EE
        };
        for (uint16_t cm : coffMachines) {
            if (machine == cm && size >= 20) {
                FormatResult r;
                r.name         = name;
                r.format       = DetectedFormat::COFF;
                r.architecture = peMachineToArch(machine);
                r.endianness   = Endianness::Little;
                r.addressSize  = (machine == 0x8664 || machine == 0xAA64 || machine == 0x0200) ? 64 : 32;
                return r;
            }
        }
    }

    // Fallback: raw binary
    FormatResult r;
    r.name   = name;
    r.format = DetectedFormat::Raw;
    return r;
}

FormatResult FormatLattice::classifyFile(const std::string &path) const
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        FormatResult r; r.name = path; return r;
    }
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    if (!f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz))) {
        FormatResult r; r.name = path; return r;
    }
    return classify(buf.data(), buf.size(), path);
}

} // namespace lattice
} // namespace fileformat
} // namespace retdec
