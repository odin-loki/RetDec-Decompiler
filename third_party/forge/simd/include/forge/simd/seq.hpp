#pragma once
// Sequence-mixing kernels for the CPU: chunked attention over an f32 or int8 KV cache and the column-blocked
// gated delta rule. Each ISA table (scalar, avx2, avx512) is built from one source (seq_impl.hpp) over a
// 16-lane vector type whose operations are all exactly rounded IEEE single precision (add, mul, fma, max,
// int8 -> float) and whose reductions use one fixed tree, and exp is a fixed polynomial written with the same
// operations. So every table returns identical bits, and results never depend on how tokens were batched.
//
// Attention (forge::graph::attn_cpu drives these):
//   attn_chunk   one KV head, positions [c0, c1) of one chunk, `ns` query rows ("slots") of that head's GQA
//                group. Each K and V row is converted once for all slots. Slot s sees positions [lo[s], hi[s]).
//                Leaves per slot the chunk's max score m, its sum of exponentials l and its unnormalised
//                weighted value sum acc[dv] (the flash-decoding partial).
//   attn_merge   folds one chunk partial into a running (M, L, A); chunks are merged in position order.
//   attn_finish  out = A * (1 / L).
// Scores are q.k * kscale * scale (kscale = 1 for f32 keys); weights below e^-80 of the chunk max are 0.
#include <cstddef>
#include <cstdint>

namespace forge::simd {

// One KV head of a cache: position j's row starts at base + j * stride (floats). int8 rows: word 0 is the f32
// scale, then the d bytes (forge::graph kv_codec int8 layout).
struct seq_kv { const float* base; std::size_t stride; int int8; };

struct seq_kernels {
    const char* target;
    // scratch floats attn_chunk needs for `ns` slots
    std::size_t (*attn_scratch)(std::size_t ns, std::size_t d, std::size_t dv);
    void (*attn_chunk)(const seq_kv& k, const seq_kv& v, std::size_t d, std::size_t dv, float scale, std::size_t c0, std::size_t c1,
                       std::size_t ns, const float* const* q, const std::size_t* lo, const std::size_t* hi,
                       float* m, float* l, float* acc, float* scratch);
    void (*attn_merge)(float* M, float* L, float* A, float m, float l, const float* acc, std::size_t dv);
    void (*attn_finish)(const float* A, float L, float* out, std::size_t dv);
    // y = x * f, f = scale / sqrt(sum x^2 + 1e-6) when l2 else scale (q/k normalisation of the delta rule)
    void (*l2norm_scale)(const float* x, float* y, std::size_t n, float scale, int l2);
    // Gated delta rule for value columns [j0, j0 + nj) (nj <= 16) of one value head over s tokens:
    //   S[:, j] *= exp(g_t);  delta_j = (v_tj - S[:, j] . k_t) * beta_t;  S[:, j] += k_t delta_j;  out_tj = S[:, j] . q_t
    // S is the head's [dk][dv] state; q/k are already normalised and scaled (token t at q + t * qk_stride),
    // v and out rows at v + t * v_stride, out + t * o_stride, g/beta at g[t * gb_stride].
    void (*delta_cols)(const float* q, const float* k, std::size_t qk_stride, const float* v, std::size_t v_stride,
                       const float* g, const float* beta, std::size_t gb_stride, float* S, float* out, std::size_t o_stride,
                       std::size_t s, std::size_t dk, std::size_t dv, std::size_t j0, std::size_t nj);
    float (*exp1)(float);   // the tables' exp (deterministic, same bits on every target)
};

const seq_kernels& seq_active();                  // FORGE_SIMD_TARGET picks scalar/avx2/avx512 like simd::active()
const seq_kernels* seq_by_name(const char* t);    // nullptr when not built / not usable here

inline constexpr std::size_t seq_attn_chunk = 256;   // positions per attention chunk (absolute: chunk c = [256c, 256c+256))

} // namespace forge::simd
