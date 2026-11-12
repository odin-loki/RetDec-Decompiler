#if defined(__aarch64__) && defined(__linux__)
#include <sys/prctl.h>
#endif
#include <forge/simd/dispatch.hpp>
#include <cstdlib>
#include <cstring>
#include <format>
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace forge::simd {
namespace mv_scalar { extern const kernels table; }
namespace mv_avx2 { extern const kernels table; }
namespace mv_avx512 { extern const kernels table; }
#if defined(__aarch64__)
namespace mv_neon { extern const kernels table; }
#endif
// Fixed-length scalable tables, one per built length (FORGE_SIMD_SVE<B> / FORGE_SIMD_RVV<B> set by CMake).
#if defined(FORGE_SIMD_SVE128)
namespace mv_sve128 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_SVE256)
namespace mv_sve256 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_SVE512)
namespace mv_sve512 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_SVE1024)
namespace mv_sve1024 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_RVV128)
namespace mv_rvv128 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_RVV256)
namespace mv_rvv256 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_RVV512)
namespace mv_rvv512 { extern const kernels table; }
#endif
#if defined(FORGE_SIMD_RVV1024)
namespace mv_rvv1024 { extern const kernels table; }
#endif
// Read the hardware vector length without SVE/V codegen flags in this TU (raw encodings, numeric CSR).
#if defined(__aarch64__) && defined(__linux__)
static int sve_vl_bytes() { long v; __asm__ volatile(".inst 0x04bf5020\n\tmov %0, x0" : "=r"(v) : : "x0"); return int(v); }  // rdvl x0, #1
#endif
#if defined(__riscv) && defined(__linux__)
static int rvv_vlenb() { unsigned long v; __asm__ volatile("csrr %0, 0xc22" : "=r"(v)); return int(v); }  // vlenb
#endif
static const kernels* sve_table(int bytes) {
    switch (bytes * 8) {
#if defined(FORGE_SIMD_SVE128)
    case 128: return &mv_sve128::table;
#endif
#if defined(FORGE_SIMD_SVE256)
    case 256: return &mv_sve256::table;
#endif
#if defined(FORGE_SIMD_SVE512)
    case 512: return &mv_sve512::table;
#endif
#if defined(FORGE_SIMD_SVE1024)
    case 1024: return &mv_sve1024::table;
#endif
    default: (void)bytes; return nullptr;
    }
}
// SVE hardware can have any VL that is a multiple of 128 bits (384, 640, ...), but fixed-length kernels exist
// only for powers of two. Linux lets a process shorten its own VL, so at load time (before main, before
// any thread exists; new threads copy it) a VL with no table (384, 640, 2048, ...) is lowered to the largest
// power of two we have a table for. FORGE_SVE_KEEP_VL=1 disables this (the sve lane then falls back to NEON).
#if defined(__aarch64__) && defined(__linux__)
#ifndef PR_SVE_SET_VL
#define PR_SVE_SET_VL 50
#endif
static int g_sve_hw_bytes = 0;   // VL before any adjustment (0 = no SVE / not adjusted)
__attribute__((constructor)) static void forge_sve_normalise_vl() {
    if (!(getauxval(AT_HWCAP) & (1ul << 22))) return;
    int vl = sve_vl_bytes();
    g_sve_hw_bytes = vl;
    if (sve_table(vl)) return;
    if (const char* e = std::getenv("FORGE_SVE_KEEP_VL"); e && *e == '1') return;
    int want = 0;
    for (int b : {16, 32, 64, 128}) if (b <= vl && sve_table(b)) want = b;
    // Threads created later copy the caller's VL; PR_SVE_VL_INHERIT (keep it across execve) is not needed.
    if (want) prctl(PR_SVE_SET_VL, want, 0, 0, 0);
}
#endif
static const kernels* rvv_table(int bytes) {
    switch (bytes * 8) {
#if defined(FORGE_SIMD_RVV128)
    case 128: return &mv_rvv128::table;
#endif
#if defined(FORGE_SIMD_RVV256)
    case 256: return &mv_rvv256::table;
#endif
#if defined(FORGE_SIMD_RVV512)
    case 512: return &mv_rvv512::table;
#endif
#if defined(FORGE_SIMD_RVV1024)
    case 1024: return &mv_rvv1024::table;
#endif
    default: (void)bytes; return nullptr;
    }
}

#if defined(__x86_64__)
static unsigned long long xgetbv0() {
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<unsigned long long>(edx) << 32) | eax;
}
#endif

static cpu_features do_detect() {
    cpu_features f;
#if defined(__x86_64__)
    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d);
    unsigned maxl = a;
    char v[13]; std::memcpy(v, &b, 4); std::memcpy(v + 4, &d, 4); std::memcpy(v + 8, &c, 4); v[12] = 0;
    f.vendor = v;
    __cpuid(1, a, b, c, d);
    f.sse2 = d >> 26 & 1; f.sse4_2 = c >> 20 & 1; f.popcnt = c >> 23 & 1; f.fma = c >> 12 & 1; f.f16c = c >> 29 & 1;
    bool osxsave = c >> 27 & 1, avx_hw = c >> 28 & 1;
    unsigned long long xcr0 = osxsave ? xgetbv0() : 0;
    f.os_avx = (xcr0 & 0x6) == 0x6;
    f.os_avx512 = f.os_avx && (xcr0 & 0xe0) == 0xe0;
    f.avx = avx_hw && f.os_avx;
    if (!f.os_avx) f.fma = f.f16c = false;
    if (maxl >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        f.avx2 = (b >> 5 & 1) && f.os_avx; f.bmi2 = b >> 8 & 1;
        if (f.os_avx512) {
            f.avx512f = b >> 16 & 1; f.avx512dq = b >> 17 & 1; f.avx512ifma = b >> 21 & 1; f.avx512cd = b >> 28 & 1;
            f.avx512bw = b >> 30 & 1; f.avx512vl = b >> 31 & 1; f.avx512vbmi = c >> 1 & 1; f.avx512vbmi2 = c >> 6 & 1;
            f.avx512vnni = c >> 11 & 1; f.avx512bitalg = c >> 12 & 1; f.avx512vpopcntdq = c >> 14 & 1;
            f.avx512fp16 = d >> 23 & 1;
        }
        f.hybrid = d >> 15 & 1;
        bool amx_tile_hw = d >> 24 & 1;
        f.amx_bf16 = d >> 22 & 1; f.amx_int8 = d >> 25 & 1;
        unsigned a1, b1, c1, d1;
        __cpuid_count(7, 1, a1, b1, c1, d1);
        f.avxvnni = (a1 >> 4 & 1) && f.os_avx; f.avx512bf16 = (a1 >> 5 & 1) && f.os_avx512; f.amx_fp16 = a1 >> 21 & 1;
        f.avx10 = d1 >> 19 & 1;
        if (f.avx10 && maxl >= 0x24) { __cpuid_count(0x24, 0, a, b, c, d); f.avx10_version = int(b & 0xff); }
        // AMX: XCR0 bits 17/18 must be enabled and Linux requires a permission request.
        f.amx_tile = amx_tile_hw && (xcr0 & 0x60000) == 0x60000;
#if defined(__linux__)
        if (f.amx_tile) {
            constexpr int ARCH_REQ_XCOMP_PERM = 0x1023, XFEATURE_XTILEDATA = 18;
            f.amx_permitted = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILEDATA) == 0;
        }
#endif
        if (!f.amx_tile) f.amx_bf16 = f.amx_int8 = f.amx_fp16 = false;
    }
    __cpuid(0x80000000, a, b, c, d);
    if (a >= 0x80000004) {
        char br[49] = {};
        for (unsigned i = 0; i < 3; ++i) { __cpuid(0x80000002 + i, a, b, c, d);
            std::memcpy(br + 16 * i, &a, 4); std::memcpy(br + 16 * i + 4, &b, 4);
            std::memcpy(br + 16 * i + 8, &c, 4); std::memcpy(br + 16 * i + 12, &d, 4); }
        f.brand = br;
    }
#elif defined(__aarch64__)
    f.neon = true; // mandatory in AArch64
    f.vendor = "aarch64";
#if defined(__linux__)
    unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
    f.sve = hw & (1ul << 22); f.sve2 = hw2 & (1ul << 1);
    if (f.sve) { f.sve_bytes = sve_vl_bytes(); f.sve_hw_bytes = g_sve_hw_bytes ? g_sve_hw_bytes : f.sve_bytes; }
#endif
#elif defined(__riscv)
    f.vendor = "riscv64";
#if defined(__linux__)
    f.rvv = getauxval(AT_HWCAP) & (1ul << ('V' - 'A'));
    if (f.rvv) f.rvv_bytes = rvv_vlenb();
#endif
#endif
    return f;
}

const cpu_features& detect() { static const cpu_features f = do_detect(); return f; }

std::string cpu_features::summary() const {
    std::string s = std::format("{} | {}", vendor, brand);
    auto add = [&](bool b, const char* n) { if (b) { s += ' '; s += n; } };
    add(sse4_2, "sse4.2"); add(avx, "avx"); add(avx2, "avx2"); add(fma, "fma"); add(f16c, "f16c");
    add(avx512f, "avx512f"); add(avx512bw, "avx512bw"); add(avx512dq, "avx512dq"); add(avx512vl, "avx512vl");
    add(avx512vnni, "avx512vnni"); add(avx512bf16, "avx512bf16"); add(avx512fp16, "avx512fp16");
    add(avx512ifma, "avx512ifma"); add(avx512vbmi, "avx512vbmi"); add(avxvnni, "avxvnni"); add(avx10, "avx10");
    add(amx_tile, "amx-tile"); add(amx_permitted, "(amx-permitted)"); add(hybrid, "hybrid"); add(neon, "neon");
    add(sve, "sve"); add(sve2, "sve2"); if (sve_bytes) s += std::format("(vl={}b)", sve_bytes * 8);
    if (sve_hw_bytes && sve_hw_bytes != sve_bytes) s += std::format("(hw vl={}b, lowered)", sve_hw_bytes * 8);
    add(rvv, "rvv"); if (rvv_bytes) s += std::format("(vlen={}b)", rvv_bytes * 8);
    return s;
}

const kernels* by_name(const char* t) {
    const auto& f = detect();
    std::string_view n = t;
    if (n == "scalar") return &mv_scalar::table;
#if defined(__aarch64__)
    if (n == "neon") return f.neon ? &mv_neon::table : nullptr;
    if (n == "sve") return f.sve ? sve_table(f.sve_bytes) : nullptr;  // table built for exactly this VL, if any
    return nullptr;
#elif defined(__riscv)
    if (n == "rvv") return f.rvv ? rvv_table(f.rvv_bytes) : nullptr;
    return nullptr;
#else
    // The whole x86-64-v3 / -v4 feature set the TUs are built for: nested VMs (e.g. the FreeBSD CI guest on AMD)
    // can report AVX2 while masking F16C/BMI2, and a kernel using them then dies with SIGILL.
    const bool v3 = f.sse2 && f.sse4_2 && f.popcnt && f.avx && f.avx2 && f.fma && f.f16c && f.bmi2;
    if (n == "avx2") return v3 ? &mv_avx2::table : nullptr;
    if (n == "avx512") return v3 && f.avx512f && f.avx512bw && f.avx512dq && f.avx512vl && f.avx512cd ? &mv_avx512::table : nullptr;
    return nullptr;
#endif
}

const kernels& active() {
    static const kernels* k = [] {
        // FORGE_LANE_TARGET is the old name, still honoured
        for (const char* var : {"FORGE_SIMD_TARGET", "FORGE_LANE_TARGET"})
            if (const char* e = std::getenv(var))
                if (auto* p = by_name(e)) return p;
        for (const char* t : {"avx512", "avx2", "sve", "rvv", "neon"}) if (auto* p = by_name(t)) return p;
        return &mv_scalar::table;
    }();
    return *k;
}

} // namespace forge::simd
