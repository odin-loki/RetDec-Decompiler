// Integer matvec: the scalar reference table (every architecture) and the choice of table for this CPU.
#include <forge/simd/qdot_i8.hpp>
#include <forge/simd/dispatch.hpp>
#include <cstdlib>
#include <string_view>

namespace forge::simd {
namespace qi8_tables {
#if defined(__x86_64__) || defined(_M_X64)
extern const qi8_kernels avx2;
#if defined(FORGE_QI8_HAVE_AVXVNNI)
extern const qi8_kernels avxvnni;
#endif
#if defined(FORGE_QI8_HAVE_AVX512VNNI)
extern const qi8_kernels avx512vnni;
#endif
#endif
} // namespace qi8_tables

int qi8_prefetch_bytes = [] {
    const char* e = std::getenv("FORGE_QI8_PF");
    return e ? std::atoi(e) : 2048;
}();

namespace qi8::scalar {
static void quantize(int fmt, const float* x, std::size_t k, void* out) {
    if (qfmt(fmt) == qfmt::q8_0) quant_32_ref(x, k, static_cast<act_32*>(out));
    else quant_k_ref(x, k, static_cast<act_k*>(out));
}
static void matvec(int fmt, const u8* w, std::size_t rb, std::size_t rows, const void* act, std::size_t as, std::size_t k,
                   std::size_t nv, float* y, std::size_t ys) {
    const auto* a = static_cast<const u8*>(act);
    for (std::size_t v = 0; v < nv; ++v)
        for (std::size_t r = 0; r < rows; ++r) {
            const u8* row = w + r * rb; const u8* av = a + v * as; float& o = y[v * ys + r];
            switch (qfmt(fmt)) {
                case qfmt::q8_0: o = dot_q8_0_ref(row, reinterpret_cast<const act_32*>(av), k / 32); break;
                case qfmt::q4_k: o = dot_q4_k_ref(row, reinterpret_cast<const act_k*>(av), k / 256); break;
                case qfmt::q5_k: o = dot_q5_k_ref(row, reinterpret_cast<const act_k*>(av), k / 256); break;
                case qfmt::q6_k: o = dot_q6_k_ref(row, reinterpret_cast<const act_k*>(av), k / 256); break;
                default: o = 0; break;
            }
        }
}
} // namespace qi8::scalar
namespace qi8_tables { static const qi8_kernels scalar = {"scalar", &qi8::scalar::quantize, &qi8::scalar::matvec}; }

const qi8_kernels* qi8_by_name(const char* t) {
    std::string_view n = t;
    if (n == "scalar") return &qi8_tables::scalar;
#if defined(__x86_64__) || defined(_M_X64)
    const auto& f = detect();
    const bool v3 = f.sse2 && f.sse4_2 && f.popcnt && f.avx && f.avx2 && f.fma && f.f16c && f.bmi2;
    if (n == "avx2") return v3 ? &qi8_tables::avx2 : nullptr;
#if defined(FORGE_QI8_HAVE_AVXVNNI)
    if (n == "avxvnni") return v3 && f.avxvnni ? &qi8_tables::avxvnni : nullptr;
#endif
#if defined(FORGE_QI8_HAVE_AVX512VNNI)
    if (n == "avx512vnni") return v3 && f.avx512f && f.avx512bw && f.avx512vl && f.avx512vnni ? &qi8_tables::avx512vnni : nullptr;
#endif
#endif
    return nullptr;
}

const qi8_kernels& qi8_active() {
    static const qi8_kernels* k = [] {
        if (const char* e = std::getenv("FORGE_QI8_TARGET"))
            if (auto* p = qi8_by_name(e)) return p;
        // FORGE_SIMD_TARGET=scalar / avx2 also caps the integer kernels (whole-process ISA bisecting)
        if (const char* e = std::getenv("FORGE_SIMD_TARGET")) {
            std::string_view s = e;
            if (s == "scalar") return &qi8_tables::scalar;
            if (s == "avx2") if (auto* p = qi8_by_name("avx2")) return p;
        }
        for (const char* t : {"avx512vnni", "avxvnni", "avx2"}) if (auto* p = qi8_by_name(t)) return p;
        return &qi8_tables::scalar;
    }();
    return *k;
}

} // namespace forge::simd
