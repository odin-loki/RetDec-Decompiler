#!/usr/bin/env python3
"""Curation tool for ops/intrinsics/x86.json.

The database records facts about the public x86 intrinsic interface (name, return type, parameter
types, required CPUID feature). It is a curated subset — the production pipeline replaces this with
an importer for the full Intel intrinsics data (see docs/STATUS.md)."""
import json

db = []
def add(name, ret, params, feat):
    db.append({"name": name, "ret": ret, "params": params, "feature": feat})

# width families: (prefix, vector types, feature for float ops, feature for int ops)
fam = [("_mm", "__m128", "__m128d", "__m128i", "sse2", "sse2"),
       ("_mm256", "__m256", "__m256d", "__m256i", "avx", "avx2"),
       ("_mm512", "__m512", "__m512d", "__m512i", "avx512f", "avx512f")]
for p, ps, pd, si, ff, fi in fam:
    fps = "sse" if p == "_mm" else ff
    for op in ["add", "sub", "mul", "div", "min", "max"]:
        add(f"{p}_{op}_ps", ps, [ps, ps], fps if p != "_mm" else "sse2")
        add(f"{p}_{op}_pd", pd, [pd, pd], ff)
    for op in ["sqrt"]:
        add(f"{p}_{op}_ps", ps, [ps], ff); add(f"{p}_{op}_pd", pd, [pd], ff)
    if p != "_mm512":
        for op in ["and", "or", "xor", "andnot"]:
            add(f"{p}_{op}_ps", ps, [ps, ps], ff); add(f"{p}_{op}_pd", pd, [pd, pd], ff)
    for op in ["and", "or", "xor", "andnot"]:
        add(f"{p}_{op}_si{128 if p=='_mm' else (256 if p=='_mm256' else 512)}", si, [si, si], fi)
    ibw = "avx512bw" if p == "_mm512" else fi
    for w in ["epi8", "epi16"]:
        for op in ["add", "sub", "adds", "subs"]:
            add(f"{p}_{op}_{w}", si, [si, si], ibw)
    for w in ["epi32", "epi64"]:
        for op in ["add", "sub"]:
            add(f"{p}_{op}_{w}", si, [si, si], fi)
    add(f"{p}_mullo_epi16", si, [si, si], ibw)
    add(f"{p}_mullo_epi32", si, [si, si], "sse4_1" if p == "_mm" else fi)
    add(f"{p}_madd_epi16", si, [si, si], ibw)
    add(f"{p}_maddubs_epi16", si, [si, si], "ssse3" if p == "_mm" else ibw)
    add(f"{p}_cvtps_epi32", si, [ps], ff); add(f"{p}_cvtepi32_ps", ps, [si], ff)
    add(f"{p}_cvttps_epi32", si, [ps], ff)
    add(f"{p}_set1_ps", ps, ["float"], fps if p != "_mm" else "sse2"); add(f"{p}_set1_pd", pd, ["double"], ff)
    add(f"{p}_set1_epi32", si, ["int"], ff if p != "_mm512" else fi)
    add(f"{p}_setzero_ps", ps, [], ff if p != "_mm" else "sse2")
    add(f"{p}_loadu_ps", ps, ["const float*"], ff if p != "_mm" else "sse2")
    add(f"{p}_storeu_ps", "void", ["float*", ps], ff if p != "_mm" else "sse2")
    add(f"{p}_loadu_pd", pd, ["const double*"], ff)
    add(f"{p}_storeu_pd", "void", ["double*", pd], ff)
for p, ps, pd, f in [("_mm", "__m128", "__m128d", "fma"), ("_mm256", "__m256", "__m256d", "fma"), ("_mm512", "__m512", "__m512d", "avx512f")]:
    for op in ["fmadd", "fmsub", "fnmadd", "fnmsub"]:
        add(f"{p}_{op}_ps", ps, [ps, ps, ps], f); add(f"{p}_{op}_pd", pd, [pd, pd, pd], f)
# AVX-512 masked forms and specials
for op in ["add", "sub", "mul", "div", "max", "min"]:
    add(f"_mm512_mask_{op}_ps", "__m512", ["__m512", "__mmask16", "__m512", "__m512"], "avx512f")
    add(f"_mm512_maskz_{op}_ps", "__m512", ["__mmask16", "__m512", "__m512"], "avx512f")
add("_mm512_mask_blend_ps", "__m512", ["__mmask16", "__m512", "__m512"], "avx512f")
add("_mm512_maskz_loadu_ps", "__m512", ["__mmask16", "const void*"], "avx512f")
add("_mm512_mask_storeu_ps", "void", ["void*", "__mmask16", "__m512"], "avx512f")
add("_mm512_scalef_ps", "__m512", ["__m512", "__m512"], "avx512f")
add("_mm512_getexp_ps", "__m512", ["__m512"], "avx512f")
add("_mm512_rcp14_ps", "__m512", ["__m512"], "avx512f")
add("_mm512_rsqrt14_ps", "__m512", ["__m512"], "avx512f")
add("_mm512_reduce_add_ps", "float", ["__m512"], "avx512f")
add("_mm512_reduce_max_ps", "float", ["__m512"], "avx512f")
add("_mm512_maskz_add_epi8", "__m512i", ["__mmask64", "__m512i", "__m512i"], "avx512bw")
add("_mm512_mask_add_epi8", "__m512i", ["__m512i", "__mmask64", "__m512i", "__m512i"], "avx512bw")
add("_mm512_dpbusd_epi32", "__m512i", ["__m512i", "__m512i", "__m512i"], "avx512vnni")
add("_mm512_dpwssd_epi32", "__m512i", ["__m512i", "__m512i", "__m512i"], "avx512vnni")
add("_mm256_dpbusd_epi32", "__m256i", ["__m256i", "__m256i", "__m256i"], "avxvnni")
add("_mm512_popcnt_epi8", "__m512i", ["__m512i"], "avx512bitalg")
add("_mm512_popcnt_epi32", "__m512i", ["__m512i"], "avx512vpopcntdq")
add("_mm512_permutexvar_epi8", "__m512i", ["__m512i", "__m512i"], "avx512vbmi")
add("_mm512_madd52lo_epu64", "__m512i", ["__m512i", "__m512i", "__m512i"], "avx512ifma")
add("_mm512_cvtne2ps_pbh", "__m512bh", ["__m512", "__m512"], "avx512bf16")
add("_mm512_dpbf16_ps", "__m512", ["__m512", "__m512bh", "__m512bh"], "avx512bf16")
add("_mm256_cvtph_ps", "__m256", ["__m128i"], "f16c")
add("_mm256_cvtps_ph", "__m128i", ["__m256", "const int"], "f16c")
add("_mm_crc32_u32", "unsigned int", ["unsigned int", "unsigned int"], "sse4_2")
add("_mm_popcnt_u64", "long long", ["unsigned long long"], "popcnt")
add("_pdep_u64", "unsigned long long", ["unsigned long long", "unsigned long long"], "bmi2")
add("_pext_u64", "unsigned long long", ["unsigned long long", "unsigned long long"], "bmi2")
add("_mm256_permutevar8x32_ps", "__m256", ["__m256", "__m256i"], "avx2")
add("_mm256_i32gather_ps", "__m256", ["const float*", "__m256i", "const int"], "avx2")
add("_mm256_maskload_ps", "__m256", ["const float*", "__m256i"], "avx")
add("_mm256_maskstore_ps", "void", ["float*", "__m256i", "__m256"], "avx")
add("_mm256_blendv_ps", "__m256", ["__m256", "__m256", "__m256"], "avx")
add("_mm256_movemask_ps", "int", ["__m256"], "avx")
add("_mm_shuffle_epi8", "__m128i", ["__m128i", "__m128i"], "ssse3")
add("_mm256_shuffle_epi8", "__m256i", ["__m256i", "__m256i"], "avx2")
add("_mm512_shuffle_epi8", "__m512i", ["__m512i", "__m512i"], "avx512bw")
add("_tile_zero", "void", ["const int"], "amx_tile")
add("_tile_release", "void", [], "amx_tile")

seen = set(); out = []
for e in db:
    if e["name"] not in seen:
        seen.add(e["name"]); out.append(e)
json.dump({"isa": "x86", "source": "curated subset (see build_x86_db.py)", "intrinsics": out},
          open(__file__.replace("build_x86_db.py", "x86.json"), "w"), indent=1)
print(len(out), "intrinsics")
