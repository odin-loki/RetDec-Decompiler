// seq_kernels dispatch: the same target names and usability rules as the lane kernel tables (dispatch.cpp).
#include <forge/simd/dispatch.hpp>
#include <forge/simd/seq.hpp>
#include <cstdlib>
#include <string_view>

namespace forge::simd {
namespace seq_scalar { extern const seq_kernels table; }
#if defined(__x86_64__) || defined(_M_X64)
namespace seq_avx2 { extern const seq_kernels table; }
namespace seq_avx512 { extern const seq_kernels table; }
#endif

const seq_kernels* seq_by_name(const char* t) {
    std::string_view n = t ? t : "";
    if (n == "scalar") return &seq_scalar::table;
#if defined(__x86_64__) || defined(_M_X64)
    if (n == "avx2") return by_name("avx2") ? &seq_avx2::table : nullptr;
    if (n == "avx512") return by_name("avx512") ? &seq_avx512::table : nullptr;
#endif
    return nullptr;
}

const seq_kernels& seq_active() {
    static const seq_kernels* k = [] {
        for (const char* var : {"FORGE_SIMD_TARGET", "FORGE_LANE_TARGET"})
            if (const char* e = std::getenv(var))
                if (auto* p = seq_by_name(e)) return p;
        for (const char* t : {"avx512", "avx2"}) if (auto* p = seq_by_name(t)) return p;
        return &seq_scalar::table;
    }();
    return *k;
}

} // namespace forge::simd
