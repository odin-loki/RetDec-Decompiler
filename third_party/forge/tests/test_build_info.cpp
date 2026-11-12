#include "check.hpp"
#include <forge/core/build_info.hpp>
#include <set>
#include <string>
#if defined(__linux__) && !defined(__ANDROID__)
#include <elf.h>
#include <link.h>
#endif

TEST(hardening_table) {
    constexpr auto flags = forge::build_hardening();
    static_assert(flags.size() == 16);
    CHECK(forge::build_hardening_known);
    std::set<std::string_view> names;
    for (auto& f : flags) {
        CHECK(!f.name.empty());
        names.insert(f.name);
        std::printf("  %-18s %s\n", std::string(f.name).c_str(), f.enabled ? "on" : "off");
    }
    CHECK(names.size() == flags.size());
}

static bool enabled(std::string_view n) {
    for (auto& f : forge::build_hardening())
        if (f.name == n) return f.enabled;
    return false;
}

#if defined(__linux__) && !defined(__ANDROID__)
// With FORGE_HARDENING on a Linux toolchain, the running executable must carry what the report claims.
TEST(linux_binary_matches_report) {
    if (forge::detail::hardening_mask == 0) { std::printf("skip: FORGE_HARDENING off\n"); return; }
    struct seen { bool relro = false, stack_x = true, now = false, pie = false; } s;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* p) -> int {
        auto& s = *static_cast<seen*>(p);   // the first object is the main program
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type == PT_GNU_RELRO) s.relro = true;
            if (ph.p_type == PT_GNU_STACK) s.stack_x = (ph.p_flags & PF_X) != 0;
            if (ph.p_type == PT_DYNAMIC) {
                for (auto* d = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + ph.p_vaddr); d->d_tag != DT_NULL; ++d) {
                    if (d->d_tag == DT_BIND_NOW) s.now = true;
                    if (d->d_tag == DT_FLAGS && (d->d_un.d_val & DF_BIND_NOW)) s.now = true;
                    if (d->d_tag == DT_FLAGS_1 && (d->d_un.d_val & DF_1_NOW)) s.now = true;
#ifdef DF_1_PIE
                    if (d->d_tag == DT_FLAGS_1 && (d->d_un.d_val & DF_1_PIE)) s.pie = true;
#endif
                }
            }
        }
        s.pie = s.pie || info->dlpi_addr != 0;   // ET_DYN main program is loaded at a non-zero bias
        return 1;
    }, &s);
    if (enabled("relro")) CHECK(s.relro);
    if (enabled("bind_now")) CHECK(s.now);
    if (enabled("noexecstack")) CHECK(!s.stack_x);
    if (enabled("pie")) CHECK(s.pie);
    CHECK(enabled("stack_protector"));
}
#endif
