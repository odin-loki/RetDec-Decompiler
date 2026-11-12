#pragma once
// Build-time hardening report (FSS-BLD-1). cmake/forge_hardening.cmake probes each flag and records the ones it
// enabled as the compile definition FORGE_HARDENING_MASK; this header decodes it. A few properties the compiler
// itself announces (stack protector, PIE, CET, BTI, fortify, STL assertions) are also reported when they come from
// the toolchain's defaults rather than from FORGE_HARDENING. Linker-only properties (RELRO, BIND_NOW, ...) can only
// come from the mask.
#include <array>
#include <cstdint>
#include <string_view>

namespace forge {

struct hardening_flag {
    std::string_view name;
    bool enabled;
};

// True when FORGE_HARDENING_MASK was defined, i.e. this TU was compiled by FORGE's own CMake build.
inline constexpr bool build_hardening_known =
#ifdef FORGE_HARDENING_MASK
    true;
#else
    false;
#endif

namespace detail {
inline constexpr std::uint32_t hardening_mask =
#ifdef FORGE_HARDENING_MASK
    std::uint32_t(FORGE_HARDENING_MASK);
#else
    0;
#endif
// Bits the compiler reports through predefined macros, whatever set them.
inline constexpr std::uint32_t hardening_predefined = 0
#if defined(__SSP_STRONG__) || defined(__SSP_ALL__)
    | (1u << 0)
#endif
#if defined(_FORTIFY_SOURCE) && _FORTIFY_SOURCE > 0 && defined(__OPTIMIZE__)
    | (1u << 1)
#endif
#if defined(__PIE__) || defined(__pie__)
    | (1u << 3)
#endif
#if defined(__CET__)
    | (1u << 7)
#endif
#if defined(__ARM_FEATURE_BTI_DEFAULT) || defined(__ARM_FEATURE_PAC_DEFAULT)
    | (1u << 8)
#endif
#if defined(_GLIBCXX_ASSERTIONS)
    | (1u << 9)
#endif
    ;
} // namespace detail

// Every hardening property FORGE knows about and whether this build has it. Names are stable identifiers.
constexpr std::array<hardening_flag, 16> build_hardening() {
    constexpr std::array<std::string_view, 16> names = {
        "stack_protector", "fortify_source", "stack_clash", "pie",         "relro",       "bind_now",
        "noexecstack",     "cf_protection",  "branch_protection", "stl_assertions", "guard_cf", "cet_compat",
        "sdl",             "dynamicbase",    "nxcompat",    "high_entropy_va"};
    constexpr std::uint32_t mask = detail::hardening_mask | detail::hardening_predefined;
    std::array<hardening_flag, 16> out{};
    for (std::size_t i = 0; i < names.size(); ++i) out[i] = {names[i], ((mask >> i) & 1u) != 0};
    return out;
}

} // namespace forge
