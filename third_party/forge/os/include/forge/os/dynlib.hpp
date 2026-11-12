#pragma once
// forge::os::dynlib: dynamic libraries with search paths, typed lookup, the loaded file's path and symbol enumeration.
//
//   auto m = os::dynlib::open("forge_plugin", {.search_dirs = {plugin_dir}});   // plugin_dir/libforge_plugin.so, then the system loader
//   auto add = m->get<int (*)(int, int)>("add");                                 // nullptr when missing
//   for (auto& s : m->symbols().value) std::printf("%s %p\n", s.name.c_str(), s.address);
//   auto syms = os::file_symbols("/usr/lib/x86_64-linux-gnu/libm.so.6");          // without loading it
//
// Built on os::library (os.hpp), which stays the plain open/symbol/close wrapper; dynlib::adopt() wraps an existing one.
// Symbol tables are read by portable byte parsers, so file_symbols() reads ELF, PE and Mach-O files on any host:
//   ELF     .dynsym (+ .gnu.version / .gnu.version_d), or PT_DYNAMIC with DT_HASH / DT_GNU_HASH when sections are stripped.
//   PE      the export directory (named exports; forwarders reported as such).
//   Mach-O  the export trie (LC_DYLD_INFO[_ONLY] or LC_DYLD_EXPORTS_TRIE), else external LC_SYMTAB entries; fat files
//           pick the host slice (else the first). Leading '_' is stripped so names match dlsym().
// symbols() on a loaded library reads its file and adds the load base (ELF, found through dl_iterate_phdr), or parses the
// mapped image in memory (Windows module, Apple image incl. the dyld shared cache).
// Parsers are bounds-checked: malformed input gives an error or a shorter list, never an out-of-range read.
// macOS / iOS loaded-image code is compiled on Apple CI only; file parsing of Mach-O is tested on every host.
#include <forge/os/os.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#if defined(FORGE_OS_POSIX) && !defined(__APPLE__)
#include <link.h>
#define FORGE_OS_DYNLIB_ELF 1
#endif

namespace forge::os {

enum class binary_format { unknown, elf, pe, macho };
inline const char* binary_format_name(binary_format f) {
    switch (f) { case binary_format::elf: return "elf"; case binary_format::pe: return "pe"; case binary_format::macho: return "macho"; default: return "unknown"; }
}

struct dynlib_symbol {
    enum class kind : std::uint8_t { function, data, tls, other };
    std::string name;
    kind type = kind::other;
    bool weak = false;
    bool indirect = false;        // ELF STT_GNU_IFUNC / Mach-O stub-and-resolver: `value` is the resolver, not the target
    std::uint64_t value = 0;      // offset from the image's load base: ELF st_value, PE RVA, Mach-O vmaddr - __TEXT vmaddr
    void* address = nullptr;      // loaded libraries only (nullptr for TLS, re-exports and from file_symbols)
    std::string version;          // ELF symbol version ("GLIBC_2.2.5"), empty when unversioned
    bool default_version = true;  // ELF: false for a hidden (non-default, foo@VER) version
    std::uint32_t ordinal = 0;    // PE export ordinal
    std::string forwarder;        // PE "DLL.Func" forwarder, Mach-O re-export "libfoo.dylib:sym"
};

struct dynlib_options {
    std::vector<std::string> search_dirs;  // tried first, in order (name and decorated name)
    std::string env_var;                   // optional variable holding more dirs (':' separated, ';' on Windows)
    bool system_search = true;             // then the platform loader's own search, exactly like os::library::open
};

namespace detail_dynlib {

struct bytes {
    const std::uint8_t* p = nullptr; std::uint64_t n = 0; bool be = false;
    mutable bool bad = false;
    bool has(std::uint64_t off, std::uint64_t len) const { return off <= n && len <= n - off; }
    std::uint64_t u(std::uint64_t off, unsigned sz) const {
        if (!has(off, sz)) { bad = true; return 0; }
        std::uint64_t v = 0;
        for (unsigned i = 0; i < sz; ++i) v = be ? (v << 8) | p[off + i] : v | (std::uint64_t(p[off + i]) << (8 * i));
        return v;
    }
    std::uint8_t u8(std::uint64_t o) const { return std::uint8_t(u(o, 1)); }
    std::uint16_t u16(std::uint64_t o) const { return std::uint16_t(u(o, 2)); }
    std::uint32_t u32(std::uint64_t o) const { return std::uint32_t(u(o, 4)); }
    std::uint64_t u64(std::uint64_t o) const { return u(o, 8); }
    // NUL-terminated string at o, at most `limit` bytes past o (and within the view)
    bool cstr(std::uint64_t o, std::string& out, std::uint64_t limit = ~std::uint64_t(0)) const {
        if (o >= n) return false;
        std::uint64_t end = std::min(n, limit > n - o ? n : o + limit);
        const void* z = std::memchr(p + o, 0, std::size_t(end - o));
        if (!z) return false;
        out.assign(reinterpret_cast<const char*>(p + o), std::size_t(static_cast<const std::uint8_t*>(z) - (p + o)));
        return true;
    }
    bytes sub(std::uint64_t off, std::uint64_t len) const {
        bytes b; if (!has(off, len)) { b.bad = true; return b; }
        b.p = p + off; b.n = len; b.be = be; return b;
    }
};

inline constexpr std::size_t max_symbols = 4u << 20;   // sanity cap for corrupt counts

inline binary_format sniff(const bytes& b) {
    if (b.n >= 4 && b.p[0] == 0x7f && b.p[1] == 'E' && b.p[2] == 'L' && b.p[3] == 'F') return binary_format::elf;
    if (b.n >= 2 && b.p[0] == 'M' && b.p[1] == 'Z') return binary_format::pe;
    if (b.n >= 4) {
        std::uint32_t m = std::uint32_t(b.p[0]) | std::uint32_t(b.p[1]) << 8 | std::uint32_t(b.p[2]) << 16 | std::uint32_t(b.p[3]) << 24;
        if (m == 0xfeedfaceu || m == 0xfeedfacfu || m == 0xbebafecau || m == 0xbfbafecau) return binary_format::macho;
    }
    return binary_format::unknown;
}

using syms = std::vector<dynlib_symbol>;
inline error bad_format(std::string_view what) { return {-1, std::string(what)}; }

// ---------------------------------------------------------------- ELF
inline result<syms> elf(bytes b) {
    if (b.n < 52 || sniff(b) != binary_format::elf) return {{}, bad_format("not an ELF file")};
    const bool c64 = b.p[4] == 2;
    if (b.p[4] != 1 && b.p[4] != 2) return {{}, bad_format("ELF: bad class")};
    if (b.p[5] != 1 && b.p[5] != 2) return {{}, bad_format("ELF: bad data encoding")};
    b.be = b.p[5] == 2;
    auto W = [&](std::uint64_t o) { return c64 ? b.u64(o) : b.u32(o); };   // address-sized word
    const std::uint64_t phoff = W(c64 ? 32 : 28), shoff = W(c64 ? 40 : 32);
    const unsigned phentsz = b.u16(c64 ? 54 : 42), phnum = b.u16(c64 ? 56 : 44), shentsz = b.u16(c64 ? 58 : 46), shnum = b.u16(c64 ? 60 : 48);
    const unsigned symsz = c64 ? 24 : 16;

    std::uint64_t symoff = 0, nsym = 0, stroff = 0, strsz = 0, versym = 0, verdef = 0, verdefnum = 0;
    bool have_versym = false, have_verdef = false, found = false;
    // 1. section headers
    if (shoff && shnum && shentsz >= (c64 ? 64u : 40u)) {
        auto sh = [&](unsigned i, unsigned f64, unsigned f32, bool word) { std::uint64_t o = shoff + std::uint64_t(i) * shentsz; return word ? W(o + (c64 ? f64 : f32)) : b.u32(o + (c64 ? f64 : f32)); };
        for (unsigned i = 0; i < shnum && !b.bad; ++i) {
            std::uint32_t type = std::uint32_t(sh(i, 4, 4, false));
            std::uint64_t off = sh(i, 24, 16, true), size = sh(i, 32, 20, true);
            if (type == 11 /*SHT_DYNSYM*/ && !found) {
                std::uint64_t ent = sh(i, 56, 36, true); if (ent < symsz) ent = symsz;
                std::uint32_t link = std::uint32_t(sh(i, 40, 24, false));
                if (link >= shnum) return {{}, bad_format("ELF: bad .dynsym link")};
                symoff = off; nsym = size / ent; stroff = sh(link, 24, 16, true); strsz = sh(link, 32, 20, true); found = true;
            } else if (type == 0x6fffffffu /*SHT_GNU_versym*/) { versym = off; have_versym = true; }
            else if (type == 0x6ffffffdu /*SHT_GNU_verdef*/) { verdef = off; verdefnum = sh(i, 44, 28, false); have_verdef = true; }
        }
        if (b.bad) return {{}, bad_format("ELF: truncated section headers")};
    }
    // 2. stripped section headers: PT_DYNAMIC, addresses mapped back to file offsets through PT_LOAD
    if (!found && phoff && phnum && phentsz >= (c64 ? 56u : 32u)) {
        struct seg { std::uint64_t off, va, sz; }; std::vector<seg> loads; std::uint64_t dyn = 0, dynsz = 0;
        for (unsigned i = 0; i < phnum && !b.bad; ++i) {
            std::uint64_t o = phoff + std::uint64_t(i) * phentsz; std::uint32_t t = b.u32(o);
            std::uint64_t off = W(o + (c64 ? 8 : 4)), va = W(o + (c64 ? 16 : 8)), fsz = W(o + (c64 ? 32 : 16));
            if (t == 1) loads.push_back({off, va, fsz}); else if (t == 2) { dyn = off; dynsz = fsz; }
        }
        auto v2o = [&](std::uint64_t va, std::uint64_t& out) { for (auto& s : loads) if (va >= s.va && va - s.va < s.sz) { out = s.off + (va - s.va); return true; } return false; };
        std::uint64_t sym_va = 0, str_va = 0, hash_va = 0, gnu_va = 0, versym_va = 0, verdef_va = 0;
        const unsigned de = c64 ? 16 : 8;
        for (std::uint64_t o = dyn; dynsz && o + de <= dyn + dynsz && !b.bad; o += de) {
            std::uint64_t tag = W(o), val = W(o + de / 2);
            if (tag == 0) break;
            switch (tag) {
            case 4: hash_va = val; break; case 5: str_va = val; break; case 6: sym_va = val; break; case 10: strsz = val; break;
            case 0x6ffffef5: gnu_va = val; break; case 0x6ffffff0: versym_va = val; break;
            case 0x6ffffffc: verdef_va = val; break; case 0x6ffffffd: verdefnum = val; break;
            default: break;
            }
        }
        if (sym_va && str_va && v2o(sym_va, symoff) && v2o(str_va, stroff)) {
            std::uint64_t h = 0;
            if (hash_va && v2o(hash_va, h)) nsym = b.u32(h + 4);   // nchain
            else if (gnu_va && v2o(gnu_va, h)) {
                std::uint32_t nb = b.u32(h), base = b.u32(h + 4), nbloom = b.u32(h + 8);
                std::uint64_t buckets = h + 16 + std::uint64_t(nbloom) * (c64 ? 8 : 4), chains = buckets + 4ull * nb;
                std::uint32_t last = 0;
                for (std::uint32_t i = 0; i < nb && !b.bad; ++i) last = std::max(last, b.u32(buckets + 4ull * i));
                if (last < base) nsym = base;
                else { std::uint64_t i = last - base; while (!b.bad && i < max_symbols && !(b.u32(chains + 4 * i) & 1)) ++i; nsym = base + i + 1; }
            }
            found = nsym > 0;
            if (versym_va) have_versym = v2o(versym_va, versym);
            if (verdef_va) have_verdef = v2o(verdef_va, verdef);
        }
    }
    if (!found) return {syms{}, {}};   // static or relocatable object: nothing exported dynamically
    if (nsym > max_symbols || !b.has(symoff, nsym * symsz)) return {{}, bad_format("ELF: symbol table out of range")};
    bytes str = b.sub(stroff, std::min(strsz, b.n > stroff ? b.n - stroff : 0));
    if (str.bad) return {{}, bad_format("ELF: string table out of range")};

    std::vector<std::string> vnames;   // version index -> name
    if (have_verdef) {
        std::uint64_t o = verdef;
        for (std::uint64_t k = 0; k < verdefnum && k < 0x8000 && !b.bad; ++k) {
            std::uint16_t ndx = b.u16(o + 4), cnt = b.u16(o + 6); std::uint32_t aux = b.u32(o + 12), next = b.u32(o + 16);
            if (cnt) { std::string nm; if (str.cstr(b.u32(o + aux), nm)) { if (ndx >= vnames.size()) vnames.resize(ndx + 1u); vnames[ndx] = nm; } }
            if (!next) break; o += next;
        }
    }
    syms out;
    for (std::uint64_t i = 1; i < nsym; ++i) {
        std::uint64_t o = symoff + i * symsz;
        std::uint32_t nm = b.u32(o); std::uint8_t info = b.u8(o + (c64 ? 4 : 12)), other = b.u8(o + (c64 ? 5 : 13));
        std::uint16_t shndx = b.u16(o + (c64 ? 6 : 14)); std::uint64_t val = c64 ? b.u64(o + 8) : b.u32(o + 4);
        unsigned bind = info >> 4, type = info & 15, vis = other & 3;
        if (shndx == 0 || (bind != 1 && bind != 2 && bind != 10) || (vis != 0 && vis != 3) || type == 3 || type == 4) continue;
        std::uint16_t vs = 1;
        if (have_versym) { vs = b.u16(versym + 2 * i); if (b.bad) { b.bad = false; have_versym = false; vs = 1; } }
        if ((vs & 0x7fff) == 0) continue;   // VER_NDX_LOCAL
        dynlib_symbol s;
        if (!str.cstr(nm, s.name) || s.name.empty()) continue;
        s.type = type == 2 || type == 10 ? dynlib_symbol::kind::function : type == 1 || type == 5 ? dynlib_symbol::kind::data
               : type == 6 ? dynlib_symbol::kind::tls : dynlib_symbol::kind::other;
        s.weak = bind == 2; s.indirect = type == 10; s.value = val;
        if ((vs & 0x7fff) > 1 && (vs & 0x7fff) < vnames.size()) s.version = vnames[vs & 0x7fff];
        s.default_version = !(vs & 0x8000);
        out.push_back(std::move(s));
    }
    return {std::move(out), {}};
}

// ---------------------------------------------------------------- PE (mapped = an image in memory, RVA == offset)
inline result<syms> pe(const bytes& b, bool mapped) {
    if (b.n < 64 || sniff(b) != binary_format::pe) return {{}, bad_format("not a PE file")};
    std::uint64_t nt = b.u32(0x3c);
    if (b.u32(nt) != 0x00004550u) return {{}, bad_format("PE: no PE signature")};
    std::uint64_t coff = nt + 4, opt = coff + 20;
    unsigned nsec = b.u16(coff + 2), optsz = b.u16(coff + 16), magic = b.u16(opt);
    if (magic != 0x10b && magic != 0x20b) return {{}, bad_format("PE: bad optional header")};
    std::uint64_t dirs = opt + (magic == 0x20b ? 112 : 96); std::uint32_t ndirs = b.u32(opt + (magic == 0x20b ? 108 : 92));
    std::uint32_t hdrsz = b.u32(opt + 60);
    if (b.bad) return {{}, bad_format("PE: truncated header")};
    if (ndirs < 1 || optsz < (magic == 0x20b ? 112u : 96u) + 8u) return {syms{}, {}};
    std::uint32_t erva = b.u32(dirs), esz = b.u32(dirs + 4);
    struct sec { std::uint32_t va, vsz, raw, rawsz, flags; }; std::vector<sec> secs;
    std::uint64_t st = opt + optsz;
    for (unsigned i = 0; i < nsec && i < 96; ++i) { std::uint64_t o = st + 40ull * i;
        secs.push_back({b.u32(o + 12), b.u32(o + 8), b.u32(o + 20), b.u32(o + 16), b.u32(o + 36)}); }
    if (b.bad) return {{}, bad_format("PE: truncated section table")};
    auto sec_of = [&](std::uint32_t rva) -> const sec* { for (auto& s : secs) if (rva >= s.va && rva - s.va < std::max(s.vsz, s.rawsz)) return &s; return nullptr; };
    auto r2o = [&](std::uint32_t rva, std::uint64_t& off) {
        if (mapped) { off = rva; return true; }
        if (rva < hdrsz) { off = rva; return true; }
        const sec* s = sec_of(rva); if (!s || rva - s->va >= s->rawsz) return false;
        off = std::uint64_t(s->raw) + (rva - s->va); return true;
    };
    if (!erva || !esz) return {syms{}, {}};
    std::uint64_t ed; if (!r2o(erva, ed) || !b.has(ed, 40)) return {{}, bad_format("PE: export directory out of range")};
    std::uint32_t base = b.u32(ed + 16), nfun = b.u32(ed + 20), nnames = b.u32(ed + 24);
    std::uint64_t afun, anames, aords;
    if (nnames > max_symbols || nfun > max_symbols) return {{}, bad_format("PE: export counts too large")};
    if (!nnames) return {syms{}, {}};
    if (!r2o(b.u32(ed + 28), afun) || !r2o(b.u32(ed + 32), anames) || !r2o(b.u32(ed + 36), aords))
        return {{}, bad_format("PE: export tables out of range")};
    syms out;
    for (std::uint32_t i = 0; i < nnames; ++i) {
        std::uint64_t no; std::uint16_t idx = b.u16(aords + 2ull * i);
        if (b.bad) return {{}, bad_format("PE: export name tables out of range")};
        if (idx >= nfun) continue;
        std::uint32_t frva = b.u32(afun + 4ull * idx);
        dynlib_symbol s;
        if (!r2o(b.u32(anames + 4ull * i), no) || !b.cstr(no, s.name, 4096) || s.name.empty()) continue;
        if (b.bad) return {{}, bad_format("PE: export address table out of range")};
        s.ordinal = base + idx; s.value = frva;
        if (frva >= erva && frva - erva < esz) {   // forwarder string inside the export directory
            std::uint64_t fo; if (r2o(frva, fo)) b.cstr(fo, s.forwarder, 4096);
            s.type = dynlib_symbol::kind::function;
        } else if (const sec* sc = sec_of(frva)) s.type = (sc->flags & 0x20000000u) ? dynlib_symbol::kind::function : dynlib_symbol::kind::data;
        out.push_back(std::move(s));
    }
    return {std::move(out), {}};
}

// ---------------------------------------------------------------- Mach-O
inline bool uleb(const bytes& b, std::uint64_t& o, std::uint64_t end, std::uint64_t& v) {
    v = 0; unsigned sh = 0;
    while (o < end && o < b.n) { std::uint8_t c = b.p[o++]; if (sh < 64) v |= std::uint64_t(c & 0x7f) << sh; sh += 7; if (!(c & 0x80)) return true; if (sh > 70) return false; }
    return false;
}
struct macho_layout {   // what the load commands say, before touching __LINKEDIT
    bool m64 = false; std::uint64_t text_va = 0; bool have_text = false;
    std::uint64_t le_va = 0, le_off = 0, le_size = 0; bool have_le = false;
    std::uint64_t trie_off = 0, trie_size = 0, symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    std::uint64_t iext = 0, next = 0; bool have_dysym = false;
    struct sect { std::uint64_t addr, size; bool code; }; std::vector<sect> sects;   // 1-based n_sect -> sects[n-1]
};
inline result<macho_layout> macho_commands(const bytes& h) {
    macho_layout L; std::uint32_t magic = h.u32(0);
    if (magic != 0xfeedfaceu && magic != 0xfeedfacfu) return {{}, bad_format("Mach-O: bad magic")};
    L.m64 = magic == 0xfeedfacfu;
    std::uint32_t ncmds = h.u32(16), cmdsz = h.u32(20); std::uint64_t o = L.m64 ? 32 : 28, end = o + cmdsz;
    if (h.bad || !h.has(o, cmdsz)) return {{}, bad_format("Mach-O: truncated load commands")};
    for (std::uint32_t i = 0; i < ncmds && o + 8 <= end; ++i) {
        std::uint32_t cmd = h.u32(o), sz = h.u32(o + 4);
        if (sz < 8 || o + sz > end) return {{}, bad_format("Mach-O: bad load command size")};
        if (cmd == 0x19 || cmd == 0x1) {   // LC_SEGMENT_64 / LC_SEGMENT
            const bool s64 = cmd == 0x19; std::string seg; h.cstr(o + 8, seg, 16); if (seg.size() > 16) seg.resize(16);
            std::uint64_t va = s64 ? h.u64(o + 24) : h.u32(o + 24), vsz = s64 ? h.u64(o + 32) : h.u32(o + 28);
            std::uint64_t fo = s64 ? h.u64(o + 40) : h.u32(o + 32);
            if (seg.rfind("__TEXT", 0) == 0 && seg.size() == 6) { L.text_va = va; L.have_text = true; }
            if (seg == "__LINKEDIT") { L.le_va = va; L.le_off = fo; L.le_size = vsz; L.have_le = true; }
            std::uint32_t nsect = h.u32(o + (s64 ? 64 : 48)); std::uint64_t so = o + (s64 ? 72 : 56), ssz = s64 ? 80 : 68;
            for (std::uint32_t k = 0; k < nsect && so + ssz <= o + sz && L.sects.size() < 255; ++k, so += ssz) {
                std::uint64_t a = s64 ? h.u64(so + 32) : h.u32(so + 32), n = s64 ? h.u64(so + 40) : h.u32(so + 36);
                std::uint32_t fl = h.u32(so + (s64 ? 64 : 56));
                L.sects.push_back({a, n, (fl & 0x80000000u) || (fl & 0x400u)});
            }
        } else if (cmd == 0x2) { L.symoff = h.u32(o + 8); L.nsyms = h.u32(o + 12); L.stroff = h.u32(o + 16); L.strsize = h.u32(o + 20); }
        else if (cmd == 0xb) { L.iext = h.u32(o + 16); L.next = h.u32(o + 20); L.have_dysym = true; }
        else if (cmd == 0x22 || cmd == 0x80000022u) { if (std::uint32_t ts = h.u32(o + 44)) { L.trie_off = h.u32(o + 40); L.trie_size = ts; } }
        else if (cmd == 0x80000033u) { if (std::uint32_t ts = h.u32(o + 12)) { L.trie_off = h.u32(o + 8); L.trie_size = ts; } }
        o += sz;
    }
    if (h.bad) return {{}, bad_format("Mach-O: truncated load commands")};
    return {std::move(L), {}};
}
inline dynlib_symbol::kind macho_kind(const macho_layout& L, std::uint64_t vmaddr) {
    for (auto& s : L.sects) if (vmaddr >= s.addr && vmaddr - s.addr < s.size) return s.code ? dynlib_symbol::kind::function : dynlib_symbol::kind::data;
    return dynlib_symbol::kind::other;
}
// `le` holds __LINKEDIT (or the whole file); file offsets are translated by subtracting le_base.
inline result<syms> macho_tables(const macho_layout& L, const bytes& le, std::uint64_t le_base, bool prefer_symtab = false) {
    syms out;
    auto rel = [&](std::uint64_t off, std::uint64_t& r) { if (off < le_base) return false; r = off - le_base; return true; };
    std::uint64_t t0;
    if (!prefer_symtab && L.trie_size && rel(L.trie_off, t0) && le.has(t0, L.trie_size)) {
        const std::uint64_t end = t0 + L.trie_size;
        struct item { std::uint64_t node; std::string prefix; }; std::vector<item> stack{{t0, {}}};
        std::size_t visits = 0;
        while (!stack.empty()) {
            item it = std::move(stack.back()); stack.pop_back();
            if (++visits > max_symbols || it.prefix.size() > 4096) return {{}, bad_format("Mach-O: export trie too deep")};
            std::uint64_t o = it.node, tsz;
            if (!uleb(le, o, end, tsz)) return {{}, bad_format("Mach-O: bad export trie")};
            std::uint64_t children = o + tsz;
            if (tsz) {
                std::uint64_t flags, v; dynlib_symbol s; s.name = it.prefix;
                if (!uleb(le, o, end, flags)) return {{}, bad_format("Mach-O: bad export trie")};
                if (flags & 0x8) {   // EXPORT_SYMBOL_FLAGS_REEXPORT: ordinal + imported name
                    std::uint64_t ord = 0; uleb(le, o, end, ord); std::string imp; le.cstr(o, imp, end - o);
                    if (imp.empty()) imp = it.prefix;
                    if (imp.size() > 1 && imp[0] == '_') imp.erase(0, 1);
                    s.forwarder = "#" + std::to_string(ord) + ":" + imp;   // dylib ordinal (LC_LOAD_DYLIB order) : symbol
                    s.type = dynlib_symbol::kind::other;
                } else {
                    if (!uleb(le, o, end, v)) return {{}, bad_format("Mach-O: bad export trie")};
                    s.value = v; s.indirect = (flags & 0x10) != 0;
                    unsigned k = unsigned(flags & 3);
                    s.type = k == 1 ? dynlib_symbol::kind::tls : k == 2 ? dynlib_symbol::kind::other
                           : (flags & 0x10) ? dynlib_symbol::kind::function : macho_kind(L, L.text_va + v);
                }
                s.weak = (flags & 0x4) != 0;
                if (s.name.size() > 1 && s.name[0] == '_') s.name.erase(0, 1);
                if (!s.name.empty()) out.push_back(std::move(s));
            }
            o = children; if (o >= end) return {{}, bad_format("Mach-O: bad export trie")};
            unsigned nchild = le.u8(o++);
            for (unsigned c = 0; c < nchild; ++c) {
                std::string edge; if (!le.cstr(o, edge, end - o)) return {{}, bad_format("Mach-O: bad export trie")};
                o += edge.size() + 1; std::uint64_t child;
                if (!uleb(le, o, end, child) || child == 0 || child >= L.trie_size) return {{}, bad_format("Mach-O: bad export trie")};
                stack.push_back({t0 + child, it.prefix + edge});
            }
        }
        std::sort(out.begin(), out.end(), [](const dynlib_symbol& a, const dynlib_symbol& b) { return a.value < b.value || (a.value == b.value && a.name < b.name); });
        return {std::move(out), {}};
    }
    if (!L.nsyms) return {std::move(out), {}};
    std::uint64_t so, sto; const unsigned nsz = L.m64 ? 16 : 12;
    if (L.nsyms > max_symbols || !rel(L.symoff, so) || !rel(L.stroff, sto) || !le.has(so, L.nsyms * nsz))
        return {{}, bad_format("Mach-O: symbol table out of range")};
    bytes str = le.sub(sto, std::min<std::uint64_t>(L.strsize, le.n > sto ? le.n - sto : 0));
    std::uint64_t first = 0, last = L.nsyms;
    if (L.have_dysym && L.iext <= L.nsyms && L.next <= L.nsyms - L.iext) { first = L.iext; last = L.iext + L.next; }
    for (std::uint64_t i = first; i < last; ++i) {
        std::uint64_t o = so + i * nsz;
        std::uint32_t strx = le.u32(o); std::uint8_t type = le.u8(o + 4), sect = le.u8(o + 5); std::uint16_t desc = le.u16(o + 6);
        std::uint64_t val = L.m64 ? le.u64(o + 8) : le.u32(o + 8);
        if ((type & 0xe0) || !(type & 1) || (type & 0x10) /*N_PEXT*/ || ((type & 0xe) != 0xe && (type & 0xe) != 2)) continue;
        dynlib_symbol s; if (!str.cstr(strx, s.name) || s.name.empty()) continue;
        if (s.name[0] == '_') s.name.erase(0, 1);
        s.weak = (desc & 0x80) != 0; s.value = val - L.text_va;
        s.type = (type & 0xe) == 2 ? dynlib_symbol::kind::other
               : sect && sect <= L.sects.size() ? (L.sects[sect - 1].code ? dynlib_symbol::kind::function : dynlib_symbol::kind::data)
               : dynlib_symbol::kind::other;
        if (!s.name.empty()) out.push_back(std::move(s));
    }
    return {std::move(out), {}};
}
inline result<syms> macho(const bytes& b, bool prefer_symtab = false) {
    std::uint32_t magic = b.u32(0);
    if (magic == 0xbebafecau || magic == 0xbfbafecau) {   // FAT_MAGIC / FAT_MAGIC_64 (stored big-endian)
        bytes f = b; f.be = true; const bool f64 = magic == 0xbfbafecau;
        std::uint32_t n = f.u32(4); const unsigned esz = f64 ? 32 : 20;
        if (!n || n > 64 || !f.has(8, std::uint64_t(n) * esz)) return {{}, bad_format("Mach-O: bad fat header")};
#if defined(__aarch64__) || defined(_M_ARM64)
        const std::uint32_t want = 0x0100000c;
#elif defined(__x86_64__) || defined(_M_X64)
        const std::uint32_t want = 0x01000007;
#else
        const std::uint32_t want = 0;
#endif
        std::uint32_t pick = 0;
        for (std::uint32_t i = 0; i < n; ++i) if (f.u32(8 + std::uint64_t(i) * esz) == want) { pick = i; break; }
        std::uint64_t e = 8 + std::uint64_t(pick) * esz;
        std::uint64_t off = f64 ? f.u64(e + 8) : f.u32(e + 8), sz = f64 ? f.u64(e + 16) : f.u32(e + 12);
        bytes s = b.sub(off, sz); if (s.bad) return {{}, bad_format("Mach-O: fat slice out of range")};
        if (s.n >= 4 && (s.u32(0) == 0xbebafecau || s.u32(0) == 0xbfbafecau)) return {{}, bad_format("Mach-O: nested fat file")};
        return macho(s, prefer_symtab);
    }
    auto L = macho_commands(b); if (!L) return {{}, L.err};
    return macho_tables(L.value, b, 0, prefer_symtab);
}

inline result<syms> parse(const bytes& b) {
    switch (sniff(b)) {
    case binary_format::elf: return elf(b);
    case binary_format::pe: return pe(b, false);
    case binary_format::macho: return macho(b);
    default: return {{}, bad_format("unknown binary format")};
    }
}

inline std::vector<std::string> split_dirs(std::string_view s) {
#if defined(FORGE_OS_WINDOWS)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::vector<std::string> out; std::size_t i = 0;
    while (i <= s.size()) { std::size_t j = s.find(sep, i); if (j == std::string_view::npos) j = s.size(); if (j > i) out.emplace_back(s.substr(i, j - i)); i = j + 1; }
    return out;
}
inline bool has_sep(std::string_view s) { for (char c : s) if (is_sep(c)) return true; return false; }
}  // namespace detail_dynlib

// Format of a file on disk (reads the first bytes only).
inline result<binary_format> file_format(const std::string& path) {
    auto f = file::open(path, rd); if (!f) return {{}, f.err};
    std::uint8_t h[4] = {}; auto n = f->pread(h, 4, 0); if (!n) return {{}, n.err};
    detail_dynlib::bytes b{h, std::uint64_t(n.value)}; return {detail_dynlib::sniff(b), {}};
}
// Exported symbols of an ELF / PE / Mach-O file on disk, without loading it (any format on any host).
inline result<std::vector<dynlib_symbol>> file_symbols(const std::string& path) {
    auto m = mapping::map_file(path); if (!m) return {{}, m.err};
    detail_dynlib::bytes b{m->data(), m->size()};
    auto r = detail_dynlib::parse(b);
    if (!r) r.err.what = path + ": " + r.err.what;
    return r;
}
// File names tried for `name`: itself, plus library::decorate(name) when the name has no extension ("m" -> libm.so).
inline std::vector<std::string> library_candidates(std::string_view name) {
    std::vector<std::string> c{std::string(name)};
    std::string b = basename(name);
    if (!name.empty() && b.find('.') == std::string::npos && !detail_dynlib::has_sep(name)) c.push_back(library::decorate(name));
    return c;
}
// First existing file for `name` in `dirs` (no system search). Empty dirs list -> not found.
inline result<std::string> find_library(std::string_view name, const std::vector<std::string>& dirs) {
    for (auto& d : dirs) for (auto& c : library_candidates(name)) { auto p = join(d, c); auto st = stat(p); if (st.exists && !st.is_dir) return {p, {}}; }
    return {{}, {-1, "library not found in search dirs: " + std::string(name)}};
}

class dynlib {
public:
    dynlib() = default;
    // name: "" = the running program; a path (contains a separator) is opened as is; otherwise opts.search_dirs and
    // $opts.env_var are searched for name / decorate(name), then the system loader is asked for name, then decorate(name).
    static result<dynlib> open(const std::string& name, const dynlib_options& opts = {}) {
        if (name.empty() || detail_dynlib::has_sep(name)) { auto l = library::open(name); if (!l) return {{}, l.err}; return adopt(std::move(l.value), name); }
        std::vector<std::string> dirs = opts.search_dirs;
        if (!opts.env_var.empty()) for (auto& d : detail_dynlib::split_dirs(getenv(opts.env_var))) dirs.push_back(d);
        if (auto p = find_library(name, dirs)) {
            auto l = library::open(p.value); if (!l) return {{}, l.err};
            return adopt(std::move(l.value), p.value);
        }
        if (!opts.system_search) return {{}, {-1, "library not found in search dirs: " + name}};
        error first;
        for (auto& c : library_candidates(name)) {
            auto l = library::open(c);
            if (l) return adopt(std::move(l.value), c);
            if (!first) first = l.err;
        }
        return {{}, first};
    }
    static result<dynlib> self() { return open(""); }
    // Wrap an already open os::library (takes ownership). `hint` is the name it was opened with, used when the
    // platform cannot map a handle back to a file.
    static result<dynlib> adopt(library l, const std::string& hint = {}) {
        if (!l.valid()) return {{}, {-1, "dynlib::adopt: library not open"}};
        dynlib d; d.lib_ = std::move(l); d.locate(hint); return {std::move(d), {}};
    }

    bool valid() const { return lib_.valid(); }
    void* native() const { return lib_.native(); }
    void* symbol(const char* s) const { return lib_.symbol(s); }
    void* symbol(const std::string& s) const { return lib_.symbol(s.c_str()); }
    bool has(const std::string& s) const { return symbol(s) != nullptr; }
    // get<int (*)(int)>("f") or get<int(int)>("f") -> function pointer; get<int*>("var") -> object pointer; nullptr if missing.
    template <class F> using ptr_t = std::conditional_t<std::is_function_v<F>, F*, F>;
    template <class F> ptr_t<F> get(const std::string& s) const {
        static_assert(std::is_pointer_v<ptr_t<F>>, "dynlib::get<F>: F must be a pointer or function type");
        return reinterpret_cast<ptr_t<F>>(symbol(s));
    }
    // Same, but an error naming the symbol when it is missing.
    template <class F> result<ptr_t<F>> require(const std::string& s) const {
        auto p = get<F>(s); if (!p) return {{}, {-1, "symbol not found: " + s + (path_.empty() ? "" : " in " + path_)}};
        return {p, {}};
    }
    // Absolute file the library was loaded from (the executable for open("")); empty if the platform cannot tell.
    const std::string& path() const { return path_; }
    const library& lib() const { return lib_; }
    library release() { path_.clear(); base_ = nullptr; return std::move(lib_); }
    void close() { lib_.close(); path_.clear(); base_ = nullptr; }

    // Exported symbols with their run-time addresses.
    result<std::vector<dynlib_symbol>> symbols() const {
        if (!valid()) return {{}, {-1, "dynlib::symbols: not open"}};
#if defined(FORGE_OS_WINDOWS)
        auto* base = static_cast<const std::uint8_t*>(native());
        detail_dynlib::bytes h{base, 4096};
        std::uint64_t nt = h.u32(0x3c); std::uint32_t image = h.u32(nt + 4 + 20 + 56);   // OptionalHeader.SizeOfImage
        if (h.bad || !image) return {{}, {-1, "dynlib::symbols: bad image header"}};
        auto r = detail_dynlib::pe(detail_dynlib::bytes{base, image}, true); if (!r) return r;
        for (auto& s : r.value) s.address = s.forwarder.empty() ? const_cast<std::uint8_t*>(base) + s.value : symbol(s.name);
        return r;
#elif defined(__APPLE__)
        if (!base_) return {{}, unsupported("dynlib::symbols: image not found")};
        auto* hdr = static_cast<const std::uint8_t*>(base_);
        detail_dynlib::bytes h{hdr, 32}; std::uint64_t cmdsz = h.u32(20);
        detail_dynlib::bytes all{hdr, 32 + cmdsz};
        auto L = detail_dynlib::macho_commands(all); if (!L) return {{}, L.err};
        if (!L->have_le || !L->have_text) return {{}, {-1, "dynlib::symbols: no __LINKEDIT"}};
        auto slide = std::intptr_t(hdr) - std::intptr_t(L->text_va);
        detail_dynlib::bytes le{reinterpret_cast<const std::uint8_t*>(std::intptr_t(L->le_va) + slide), L->le_size};
        auto r = detail_dynlib::macho_tables(L.value, le, L->le_off); if (!r) return r;
        for (auto& s : r.value) {
            if (!s.forwarder.empty() || s.type == dynlib_symbol::kind::tls || s.type == dynlib_symbol::kind::other) continue;
            s.address = s.indirect ? symbol(s.name) : const_cast<std::uint8_t*>(hdr) + s.value;
        }
        return r;
#elif defined(FORGE_OS_DYNLIB_ELF)
        if (path_.empty()) return {{}, {-1, "dynlib::symbols: file of this library is unknown"}};
        auto r = file_symbols(path_); if (!r) return r;
        auto b = reinterpret_cast<std::uintptr_t>(base_);
        for (auto& s : r.value) {
            if (s.type == dynlib_symbol::kind::tls) continue;
            if (!s.indirect) s.address = reinterpret_cast<void*>(b + std::uintptr_t(s.value));
            else if (s.default_version) s.address = symbol(s.name);   // let the loader run the resolver
#if defined(__GLIBC__) || defined(__FreeBSD__) || (defined(__ANDROID__) && __ANDROID_API__ >= 24)   // dlvsym: glibc, FreeBSD libc, bionic 24+
            else s.address = ::dlvsym(native(), s.name.c_str(), s.version.c_str());
#endif
        }
        return r;
#else
        return {{}, unsupported("dynlib::symbols")};
#endif
    }

private:
    library lib_; std::string path_; const void* base_ = nullptr;

    void locate(const std::string& hint) {
        const bool self = lib_.is_self();
#if defined(FORGE_OS_WINDOWS)
        (void)self; (void)hint;
        std::wstring b(512, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(static_cast<HMODULE>(native()), b.data(), DWORD(b.size()));
            if (!n) break;
            if (n < b.size()) { path_ = narrow({b.data(), n}); break; }
            if (b.size() >= 32768) break; b.resize(b.size() * 2);
        }
        base_ = native();
#elif defined(__APPLE__)
        const std::uint32_t n = _dyld_image_count();
        if (self) { path_ = exe_path(); for (std::uint32_t i = 0; i < n; ++i) if (_dyld_get_image_header(i)->filetype == 2 /*MH_EXECUTE*/) { base_ = _dyld_get_image_header(i); break; } return; }
        for (std::uint32_t i = 0; i < n && !base_; ++i) {
            const char* nm = _dyld_get_image_name(i); if (!nm) continue;
            void* h = ::dlopen(nm, RTLD_NOLOAD | RTLD_LAZY);
            if (h) { if (h == native()) { path_ = nm; base_ = _dyld_get_image_header(i); } ::dlclose(h); }
        }
        if (!base_ && !hint.empty()) {   // handle values can carry mode bits: fall back to the name
            const bool full = detail_dynlib::has_sep(hint); std::string want = full ? absolute(hint) : basename(hint);
            for (std::uint32_t i = 0; i < n && !base_; ++i) {
                const char* nm = _dyld_get_image_name(i); if (!nm) continue;
                if (full ? absolute(nm) == want : basename(nm) == want) { path_ = nm; base_ = _dyld_get_image_header(i); }
            }
        }
#elif defined(FORGE_OS_DYNLIB_ELF)
        (void)hint;
        struct obj { std::string name; std::uintptr_t addr; };
        std::vector<obj> objs;
        ::dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* ud) -> int {
            static_cast<std::vector<obj>*>(ud)->push_back({info->dlpi_name ? info->dlpi_name : "", std::uintptr_t(info->dlpi_addr)});
            return 0;
        }, &objs);
        if (self) {   // the main program is reported first
            path_ = exe_path(); if (!objs.empty()) base_ = reinterpret_cast<const void*>(objs.front().addr); return;
        }
        // dlopen is not called inside the dl_iterate_phdr callback: the loader lock may be held there.
        for (std::size_t i = 1; i < objs.size(); ++i) {
            if (objs[i].name.empty()) continue;
            void* h = ::dlopen(objs[i].name.c_str(), RTLD_NOLOAD | RTLD_LAZY);
            if (!h) continue;
            bool match = h == native(); ::dlclose(h);
            if (match) { path_ = absolute(objs[i].name); base_ = reinterpret_cast<const void*>(objs[i].addr); return; }
        }
#else
        (void)self; (void)hint;
#endif
    }
};

}  // namespace forge::os
