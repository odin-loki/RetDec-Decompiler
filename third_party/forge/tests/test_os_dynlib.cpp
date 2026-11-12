// forge::os::dynlib: search dirs, typed get<>, path(), and exported-symbol enumeration of loaded libraries and of
// ELF / PE / Mach-O files on disk. FORGE_TEST_FIXTURE_DIR holds the native fixture (tests/dynlib_fixture.c);
// FORGE_TEST_XFMT_DIR, when clang + lld were found, holds cross-linked PE / Mach-O / ELF variants of it.
#include <forge/os/dynlib.hpp>
#include "check.hpp"
#include <algorithm>
#include <cmath>
#include <set>
using namespace forge;
using kind = os::dynlib_symbol::kind;

static const os::dynlib_symbol* find(const std::vector<os::dynlib_symbol>& v, std::string_view n, std::string_view ver = {}) {
    for (auto& s : v) if (s.name == n && (ver.empty() || s.version == ver)) return &s;
    return nullptr;
}
static std::set<std::string> names(const std::vector<os::dynlib_symbol>& v) { std::set<std::string> r; for (auto& s : v) r.insert(s.name); return r; }

#if defined(FORGE_TEST_FIXTURE_DIR)
static const std::string fixture_dir = FORGE_TEST_FIXTURE_DIR;
static os::dynlib_options in(std::vector<std::string> dirs) { os::dynlib_options o; o.search_dirs = std::move(dirs); return o; }

TEST(fixture_search_get_path) {
    os::dynlib_options none; none.system_search = false;
    CHECK(!os::dynlib::open("forge_fixture", none));                                  // not found without dirs
    auto l = os::dynlib::open("forge_fixture", in({fixture_dir + "/missing", fixture_dir}));
    CHECK_MSG(l, "%s", l.err.what.c_str()); if (!l) return;
    CHECK(os::basename(l->path()) == os::library::decorate("forge_fixture"));
    CHECK(os::exists(l->path()));
    auto add = l->get<int (*)(int, int)>("forge_fixture_add"); CHECK(add && add(2, 3) == 5);
    auto name = l->get<const char*()>("forge_fixture_name"); CHECK(name && std::strcmp(name(), "forge-fixture") == 0);
    auto counter = l->get<int*>("forge_fixture_counter"); CHECK(counter && *counter == 42);
    CHECK(!l->get<int (*)(int)>("forge_fixture_hidden"));                             // hidden visibility / not dllexport
    CHECK(!l->has("forge_fixture_static"));
    auto miss = l->require<void (*)()>("forge_fixture_nope"); CHECK(!miss); CHECK(miss.err.what.find("forge_fixture_nope") != std::string::npos);
    auto ok = l->require<int(int, int)>("forge_fixture_add"); CHECK(ok && ok.value(4, 5) == 9);

    CHECK(os::setenv("FORGE_TEST_DYNLIB_PATH", fixture_dir));
    os::dynlib_options env; env.env_var = "FORGE_TEST_DYNLIB_PATH"; env.system_search = false;
    auto e = os::dynlib::open("forge_fixture", env); CHECK(e); if (e) CHECK(e->path() == l->path());
    os::unsetenv("FORGE_TEST_DYNLIB_PATH");

    auto p = os::find_library("forge_fixture", {fixture_dir}); CHECK(p);
    auto byp = os::dynlib::open(p.value); CHECK(byp); if (byp) CHECK(byp->path() == l->path());
    auto a = os::library::open(p.value); CHECK(a);
    auto ad = os::dynlib::adopt(std::move(a.value)); CHECK(ad); if (ad) { CHECK(ad->path() == l->path()); CHECK(ad->has("forge_fixture_add")); }
}

TEST(fixture_symbols_loaded_and_on_disk) {
    auto l = os::dynlib::open("forge_fixture", in({fixture_dir})); CHECK(l); if (!l) return;
    auto s = l->symbols(); CHECK_MSG(s, "%s", s.err.what.c_str()); if (!s) return;
    std::printf("  %s: %zu exported symbols\n", l->path().c_str(), s->size());
    auto add = find(*s, "forge_fixture_add"), nm = find(*s, "forge_fixture_name"), ctr = find(*s, "forge_fixture_counter");
    CHECK(add && nm && ctr); if (!add || !nm || !ctr) return;
    CHECK(add->type == kind::function); CHECK(nm->type == kind::function); CHECK(ctr->type == kind::data);
    CHECK(add->address == l->symbol("forge_fixture_add"));
    CHECK(ctr->address == l->get<int*>("forge_fixture_counter"));
    CHECK(reinterpret_cast<int (*)(int, int)>(add->address)(10, 20) == 30);   // call through the enumerated address
    CHECK(!find(*s, "forge_fixture_hidden")); CHECK(!find(*s, "forge_fixture_static"));

    auto f = os::file_symbols(l->path()); CHECK_MSG(f, "%s", f.err.what.c_str()); if (!f) return;
    CHECK(names(*f) == names(*s));
    for (auto& x : *f) { CHECK(x.address == nullptr); if (auto y = find(*s, x.name)) CHECK(y->value == x.value); }
    auto fmt = os::file_format(l->path()); CHECK(fmt);
#if defined(_WIN32)
    CHECK(fmt.value == os::binary_format::pe);
#elif defined(__APPLE__)
    CHECK(fmt.value == os::binary_format::macho);
#else
    CHECK(fmt.value == os::binary_format::elf);
#endif
}
#endif

TEST(system_math_library) {
#if defined(_WIN32)
    const char* lib = "msvcrt.dll";
#elif defined(__APPLE__)
    const char* lib = "/usr/lib/system/libsystem_m.dylib";
#elif defined(__ANDROID__)
    const char* lib = "libm.so";
#elif defined(__FreeBSD__)
    const char* lib = "libm.so.5";
#else
    const char* lib = "libm.so.6";
#endif
    auto m = os::dynlib::open(lib); CHECK_MSG(m, "%s", m.err.what.c_str()); if (!m) return;
    auto cosf = m->get<double (*)(double)>("cos"); CHECK(cosf && cosf(0.0) == 1.0);
    CHECK(!m->path().empty());
    auto s = m->symbols(); CHECK_MSG(s, "%s", s.err.what.c_str()); if (!s) return;
    std::printf("  %s (%s): %zu exported symbols\n", lib, m->path().c_str(), s->size());
    const os::dynlib_symbol* c = nullptr;
    for (auto& x : *s) if (x.name == "cos" && x.default_version) c = &x;
    CHECK(c); if (!c) return;
    CHECK(c->type == kind::function);
    CHECK(c->address == m->symbol("cos"));   // IFUNCs (glibc x86_64 cos) resolve through the loader
    CHECK(reinterpret_cast<double (*)(double)>(c->address)(0.0) == 1.0);
#if defined(__GLIBC__)
    CHECK(c->version.rfind("GLIBC_", 0) == 0);
#endif
#if defined(__GLIBC__) && defined(__x86_64__)
    auto libc = os::file_symbols(os::join(os::dirname(m->path()), "libc.so.6")); CHECK(libc);
    bool hidden_version = false; for (auto& x : libc.value) hidden_version |= !x.default_version;
    CHECK(hidden_version);   // e.g. memcpy@GLIBC_2.2.5 next to memcpy@@GLIBC_2.14
#endif
}

TEST(self_and_errors) {
    auto self = os::dynlib::self(); CHECK(self); if (!self) return;
    CHECK(!self->path().empty());
    if (!self->path().empty()) CHECK(os::absolute(self->path()) == os::absolute(os::exe_path()));
    auto s = self->symbols(); CHECK_MSG(s, "%s", s.err.what.c_str());   // an executable may export nothing
    auto bad = os::file_symbols("CMakeLists.txt"); CHECK(!bad); CHECK(bad.err.what.find("unknown binary format") != std::string::npos);
    CHECK(!os::file_symbols("definitely/missing/file.so"));
    CHECK(!os::dynlib::open("definitely_missing_forge_lib"));
    CHECK(os::library_candidates("m").size() == 2); CHECK(os::library_candidates("libm.so.6").size() == 1);
    CHECK(os::library_candidates("dir/m").size() == 1);
    os::dynlib closed; CHECK(!closed.valid()); CHECK(!closed.symbols());
}

#if defined(FORGE_TEST_XFMT_DIR)
static const std::string xdir = FORGE_TEST_XFMT_DIR;
static int checked = 0;
static std::vector<os::dynlib_symbol> xsyms(const std::string& f) {
    auto p = os::join(xdir, f);
    if (!os::exists(p)) { std::printf("  (fixture %s not built, skipped)\n", f.c_str()); return {}; }
    auto r = os::file_symbols(p); CHECK_MSG(r, "%s: %s", f.c_str(), r.err.what.c_str());
    ++checked; return r.value;
}
static void common(const std::vector<os::dynlib_symbol>& v, const char* f) {
    auto add = find(v, "forge_fixture_add"), nm = find(v, "forge_fixture_name"), ctr = find(v, "forge_fixture_counter");
    CHECK_MSG(add && nm && ctr, "%s", f); if (!add || !nm || !ctr) return;
    CHECK_MSG(add->type == kind::function && nm->type == kind::function && ctr->type == kind::data, "%s", f);
    CHECK_MSG(add->value && add->value != nm->value && ctr->value, "%s", f);
    CHECK_MSG(!find(v, "forge_fixture_hidden") && !find(v, "forge_fixture_static"), "%s", f);
}

TEST(foreign_formats_pe) {
    for (const char* f : {"pe64.dll", "pe32.dll"}) {
        auto v = xsyms(f); if (v.empty()) continue;
        common(v, f); CHECK(v.size() == 4);
        auto fwd = find(v, "forge_fixture_fwd");   // lld-link writes "_KERNEL32.GetTickCount" for i386 (stdcall decoration)
        CHECK(fwd && fwd->forwarder.ends_with("KERNEL32.GetTickCount") && fwd->type == kind::function);
        std::set<std::uint32_t> ords; for (auto& s : v) ords.insert(s.ordinal);
        CHECK(ords == (std::set<std::uint32_t>{1, 2, 3, 4}));
        CHECK(os::file_format(os::join(xdir, f)).value == os::binary_format::pe);
    }
}
TEST(foreign_formats_macho) {
    std::vector<os::dynlib_symbol> arm;
    for (const char* f : {"macho_arm64.dylib", "macho_x86_64.dylib", "macho_trie.dylib", "macho_fat.dylib"}) {
        auto v = xsyms(f); if (v.empty()) continue;
        common(v, f); CHECK_MSG(v.size() == 3, "%s has %zu", f, v.size());
        if (std::string(f) == "macho_arm64.dylib") arm = v;
        if (std::string(f) == "macho_trie.dylib" && !arm.empty()) for (auto& s : v) CHECK(find(arm, s.name));
    }
    if (arm.empty()) return;
    // The LC_SYMTAB fallback must agree with the export trie.
    auto m = os::mapping::map_file(os::join(xdir, "macho_arm64.dylib")); CHECK(m); if (!m) return;
    auto st = os::detail_dynlib::macho(os::detail_dynlib::bytes{m->data(), m->size()}, true); CHECK(st); if (!st) return;
    CHECK(names(*st) == names(arm));
    for (auto& s : *st) if (auto t = find(arm, s.name)) { CHECK(t->value == s.value); CHECK(t->type == s.type); }
}
TEST(foreign_formats_elf) {
    std::vector<os::dynlib_symbol> gnu, e32;
    for (const char* f : {"elf32.so", "elf_be.so", "elf_gnu.so", "elf_gnu_nosec.so", "elf32_nosec.so"}) {
        auto v = xsyms(f); if (v.empty()) continue;
        common(v, f); CHECK_MSG(v.size() == 3, "%s has %zu", f, v.size());
        std::string n = f;
        if (n == "elf_gnu.so") gnu = v; if (n == "elf32.so") e32 = v;
        auto& ref = n == "elf_gnu_nosec.so" ? gnu : n == "elf32_nosec.so" ? e32 : v;   // PT_DYNAMIC path == section path
        for (auto& s : v) if (auto r = find(ref, s.name)) CHECK_MSG(r->value == s.value, "%s %s", f, s.name.c_str());
    }
    auto v = xsyms("elf_ver.so"); if (v.empty()) return;
    common(v, "elf_ver.so");
    auto v2 = find(v, "forge_fixture_add", "FIXV_2"), v1 = find(v, "forge_fixture_add", "FIXV_1");
    CHECK(v2 && v2->default_version); CHECK(v1 && !v1->default_version);
    auto tw = find(v, "forge_fixture_twice"); CHECK(tw && tw->indirect && tw->type == kind::function);
    auto tls = find(v, "forge_fixture_tls"); CHECK(tls && tls->type == kind::tls);
    CHECK(!find(v, "forge_fixture_add_v1"));
}
#if ((defined(__linux__) && defined(__GLIBC__)) || defined(__FreeBSD__)) && defined(__x86_64__)
TEST(elf_versions_ifunc_tls_loaded) {   // elf_ver.so has no libc dependency, so the host loader takes it as is
    auto p = os::join(xdir, "elf_ver.so"); if (!os::exists(p)) return;
    auto l = os::dynlib::open(p);
#if defined(__FreeBSD__)
    if (!l) { std::printf("  skip: rtld refused the fixture: %s\n", l.err.what.c_str()); return; }
#endif
    CHECK_MSG(l, "%s", l.err.what.c_str()); if (!l) return;
    auto s = l->symbols(); CHECK(s); if (!s) return;
    auto tw = find(*s, "forge_fixture_twice"); CHECK(tw && tw->address == l->symbol("forge_fixture_twice"));
    if (tw && tw->address) CHECK(reinterpret_cast<int (*)(int)>(tw->address)(21) == 42);
    auto v1 = find(*s, "forge_fixture_add", "FIXV_1"), v2 = find(*s, "forge_fixture_add", "FIXV_2");
    CHECK(v1 && v1->address && v2 && v2->address && v1->address != v2->address);
    if (v1 && v1->address) CHECK(reinterpret_cast<int (*)(int, int)>(v1->address)(5, 3) == 2);
    if (v2 && v2->address) CHECK(reinterpret_cast<int (*)(int, int)>(v2->address)(5, 3) == 8);
    CHECK(l->get<int(int, int)>("forge_fixture_add")(5, 3) == 8);   // dlsym picks the default version
    auto tls = find(*s, "forge_fixture_tls"); CHECK(tls && !tls->address);
    CHECK(l->path() == os::absolute(p));
}
#endif
#endif

TEST(malformed_inputs_never_read_out_of_bounds) {
    using os::detail_dynlib::bytes;
    std::vector<std::string> files;
#if defined(FORGE_TEST_FIXTURE_DIR)
    if (auto p = os::find_library("forge_fixture", {fixture_dir})) files.push_back(p.value);
#endif
#if defined(FORGE_TEST_XFMT_DIR)
    for (const char* f : {"pe64.dll", "pe32.dll", "macho_trie.dylib", "macho_fat.dylib", "macho_x86_64.dylib", "elf_ver.so", "elf_be.so", "elf32_nosec.so"})
        if (os::exists(os::join(xdir, f))) files.push_back(os::join(xdir, f));
#endif
    for (std::string junk : {std::string(), std::string("MZ"), std::string("\x7f" "ELF\x02\x01", 6), std::string("\xcf\xfa\xed\xfe", 4),
                             std::string("\xca\xfe\xba\xbe\x00\x00\x00\x01", 8), std::string(64, '\xff')}) {
        auto r = os::detail_dynlib::parse(bytes{reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size()});
        CHECK(!r || r.value.empty());
    }
    std::uint64_t seed = 0x9e3779b97f4a7c15ull; auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    std::size_t runs = 0;
    for (auto& f : files) {
        auto data = os::read_file(f); CHECK(data); if (!data) continue;
        std::vector<std::uint8_t> orig(data->begin(), data->end());
        for (std::size_t n = 0; n < orig.size(); n += 1 + n / 16) {   // every short prefix, then sparser
            std::vector<std::uint8_t> cut(orig.begin(), orig.begin() + std::ptrdiff_t(n));   // exact-size heap copy (ASan-visible)
            (void)os::detail_dynlib::parse(bytes{cut.data(), cut.size()}); ++runs;
        }
        for (int it = 0; it < 1500; ++it) {
            std::vector<std::uint8_t> m = orig;
            for (int k = 1 + int(rnd() % 8); k > 0; --k) {
                std::size_t at = (rnd() % 4 == 0) ? std::size_t(rnd() % std::min<std::size_t>(m.size(), 512)) : std::size_t(rnd() % m.size());
                m[at] = std::uint8_t(rnd() % 3 == 0 ? 0xff : rnd());
            }
            (void)os::detail_dynlib::parse(bytes{m.data(), m.size()}); ++runs;
        }
    }
    std::printf("  %zu malformed inputs parsed across %zu files\n", runs, files.size());
}

#if defined(FORGE_TEST_XFMT_DIR)
TEST(foreign_fixture_count) { std::printf("  %d foreign-format fixtures checked\n", checked); }
#endif
