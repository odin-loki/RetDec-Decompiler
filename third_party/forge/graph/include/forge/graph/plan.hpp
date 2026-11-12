#pragma once
// partition -> memory-plan -> lower -> flat plan -> replay. Plans are cached per shape bucket.
#include <map>
#include <cstdio>
#include <algorithm>
#include <forge/graph/codecs.hpp>
#include <forge/graph/kernels.hpp>
#include <forge/simd/qdot.hpp>
#include <forge/simd/qdot_i8.hpp>
#include <forge/graph/kv_codec.hpp>
#include <forge/graph/memplan.hpp>
#include <forge/graph/passes.hpp>
#include <forge/gpu/gpu.hpp>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>

namespace forge::graph {

// Per-session state store: slot -> preallocated [capacity, ...] tensor + current length.
struct state_store {
    // Fixed slots (state_op: recurrent state, conv window) keep a constant-size buffer; `len` counts the rows
    // consumed so far, and they cannot be rewound except to 0.
    struct slot { tensor buf; i64 len = 0; i64 capacity = 0; kv_codec codec = kv_codec::f32; bool fixed = false; };
    std::map<std::string, slot> slots;
    i64 capacity = 4096;   // maximum rows per slot (context length)
    // Paged allocation: when > 0, slot storage grows in pages of `page_rows` rows on demand instead of
    // reserving `capacity` rows up front, so memory tracks the tokens actually held.
    i64 page_rows = 0;
    // KV codec for new slots (rank-3 [rows, heads, d] slots only; others stay f32). Set before the first
    // plan is compiled against this store. Quantised slots hold [rows, heads, kv_head_words(d)] floats.
    kv_codec codec = kv_codec::f32;
    // Value-slot codec for mixed modes (k8v4: int8 keys, tbq4 values); f32 here means "same as codec".
    kv_codec codec_v = kv_codec::f32;
    // Host offload: drop the device copy of every slot right after the attention that reads it, so device
    // memory holds at most one layer's KV at a time (the host copy is authoritative between steps).
    bool offload = false;
    // physical row shape for a slot fed by logical rows `xs` ([rows, heads, d])
    kv_codec codec_for(const shape_t& xs, bool value = false) const {
        if (xs.size() != 3) return kv_codec::f32;
        return value && codec_v != kv_codec::f32 ? codec_v : codec;
    }
    static shape_t physical(const shape_t& xs, kv_codec c) {
        if (c == kv_codec::f32) return xs;
        return {xs[0], xs[1], kv_head_words(xs[2], c)};
    }
    i64 reserved_rows() const { i64 r = 0; for (auto& [k, s] : slots) r += s.capacity; return r; }
    std::size_t bytes() const { std::size_t b = 0; for (auto& [k, s] : slots) b += s.buf.data.size() * sizeof(float); return b; }
    // make room for `rows` rows in `sl` (row shape taken from `row_shape`)
    void ensure(slot& sl, const shape_t& row_shape, i64 rows) {
        if (rows > capacity) raise(errc::out_of_memory, "state exceeds context capacity {}", capacity);
        if (rows <= sl.capacity) return;
        i64 want = page_rows > 0 ? std::min(capacity, (rows + page_rows - 1) / page_rows * page_rows) : capacity;
        shape_t cap = row_shape; cap[0] = want;
        tensor nb(cap);
        // the old buffer is about to be freed: sync any device-only rows back and drop the device copy keyed by it
        if (sl.capacity && hooks && hooks->release) hooks->release(sl.buf.data.data(), sl.buf.data.size());
        if (sl.capacity) std::copy_n(sl.buf.data.data(), std::size_t(numel(sl.buf.shape) / std::max<i64>(sl.capacity, 1) * sl.len), nb.data.data());
        sl.buf = std::move(nb); sl.capacity = want;
    }
    std::map<std::string, shape_t> shapes() const {
        std::map<std::string, shape_t> r;
        for (auto& [k, s] : slots) { shape_t sh = s.buf.shape; sh[0] = s.len; r[k] = sh; }
        return r;
    }
    void reset() { for (auto& [k, s] : slots) s.len = 0; }
    // Device runs may leave slot contents on the device only (device_hooks::keep): flush() brings the host copies
    // up to date, forget() also drops the device copies (before the host buffers are replaced or freed).
    device_hooks* hooks = nullptr;
    void flush() const { if (hooks && hooks->to_host) for (auto& [k, s] : slots) if (!s.buf.data.empty()) hooks->to_host(s.buf.data.data(), s.buf.data.size()); }
    void forget() {
        if (hooks && hooks->release) for (auto& [k, s] : slots) if (!s.buf.data.empty()) hooks->release(s.buf.data.data(), s.buf.data.size());
        drop_snapshots();
    }
    // ---- speculative rollback of fixed (recurrent) slots ----
    // When snap_row >= 0, the next run copies every fixed slot aside after the first snap_row rows of its state op
    // (the executor splits that op in two there). On a device backend with a device copy the copy stays on the
    // device: no host transfer. restore_snapshots() then puts the slots back as they were at that row, so a
    // speculative verify pass can be undone for the cost of one device-to-device copy (session::forward_spec).
    struct snapshot { tensor buf; i64 len = 0; };
    i64 snap_row = -1;
    std::map<std::string, snapshot> snaps;
    // called by the executor for a fixed slot `k` whose state buffer is S (n floats), `len` rows consumed
    void take_snapshot(const std::string& k, float* S, std::size_t n, i64 len, device_hooks* h) {
        auto& sn = snaps[k];
        if (sn.buf.data.size() != n) { drop_device(h, sn.buf); sn.buf = tensor(shape_t{i64(n)}); }
        sn.len = len;
        if (h && h->copy && h->keep) { h->copy(S, n, sn.buf.data.data(), n, 0); if (h->keep) h->keep(sn.buf.data.data(), n); hooks = h; return; }
        if (h) h->to_host(S, n);
        std::copy_n(S, n, sn.buf.data.data());
    }
    // put the fixed slots back as of the last snapshot; returns false (nothing changed) when there is none
    bool restore_snapshots() {
        if (snaps.empty()) return false;
        for (auto& [k, sn] : snaps) {
            auto it = slots.find(k);
            if (it == slots.end() || !it->second.fixed || it->second.buf.data.size() != sn.buf.data.size()) return false;
        }
        for (auto& [k, sn] : snaps) {
            auto& sl = slots[k]; std::size_t n = sn.buf.data.size();
            if (hooks && hooks->copy && hooks->keep) { hooks->copy(sn.buf.data.data(), n, sl.buf.data.data(), n, 0); hooks->keep(sl.buf.data.data(), n); }
            else { if (hooks && hooks->to_host) hooks->to_host(sn.buf.data.data(), n); std::copy_n(sn.buf.data.data(), n, sl.buf.data.data()); if (hooks && hooks->host_wrote) hooks->host_wrote(sl.buf.data.data(), n); }
            sl.len = sn.len;
        }
        return true;
    }
    void drop_snapshots() {
        for (auto& [k, sn] : snaps) drop_device(hooks, sn.buf);
        snaps.clear();
    }
    static void drop_device(device_hooks* h, tensor& t) {   // the device copy of a snapshot, unread
        if (!h || t.data.empty()) return;
        if (h->forget) h->forget(t.data.data(), t.data.size()); else if (h->release) h->release(t.data.data(), t.data.size());
    }
    state_store() = default;
    state_store(const state_store& o) : slots((o.flush(), o.slots)), capacity(o.capacity), page_rows(o.page_rows), codec(o.codec), codec_v(o.codec_v), offload(o.offload) {}
    state_store& operator=(const state_store&) = delete;
    ~state_store() { try { forget(); } catch (...) {} }
    // rewind all slots to `len` rows (e.g. prefix reuse / speculative rollback)
    void truncate(i64 len) {
        for (auto& [k, s] : slots) {
            if (s.fixed && len > 0 && len < s.len)
                raise(errc::unsupported, "state slot '{}' is recurrent and cannot be rewound to {} (has {})", k, len, s.len);
            s.len = std::min(s.len, len);
        }
    }
};

struct partition_report {
    struct dev { std::string name; double flops = 0; double bytes = 0; int nodes = 0; };
    std::vector<dev> devices;
};

inline double node_flops(const tensor_graph& g, const node& n) {
    if (n.kind != node_kind::op) return 0;
    const auto& inf = ops::info(n.op);
    double out = double(numel(n.shape));
    if (inf.cls == ops::op_class::contraction && !n.inputs.empty()) {
        if (n.op == ops::op::moe_ffn && n.inputs.size() == 5)   // k experts x 3 projections of f x d per token
            return 6.0 * double(numel(n.shape)) * n.attrs.num("k", 2) * double(g.at(n.inputs[2]).shape[1]);
        if (n.op == ops::op::attention) return 4.0 * out * double(g.at(n.inputs[1]).shape[0]);
        return 2.0 * out * double(g.at(n.inputs[0]).shape.back());
    }
    return out * inf.cost;
}

// Capability + cost partitioner. With one device all nodes land there; with several, contiguous
// layer ranges are balanced by flops (pipeline split). Nodes whose op has no lowering on a device
// are never assigned to it.
inline partition_report partition(tensor_graph& g, const std::vector<gpu::device>& devs) {
    partition_report r;
    for (auto& d : devs) r.devices.push_back({d.caps().name});
    double total = 0; for (auto& n : g.nodes()) total += node_flops(g, n);
    double acc = 0;
    for (auto& n : g.nodes()) {
        double f = node_flops(g, n);
        int d = devs.size() <= 1 ? 0 : std::min(int(devs.size()) - 1, int(acc / std::max(total, 1.0) * double(devs.size())));
        n.device = d; acc += f;
        r.devices[std::size_t(d)].flops += f;
        r.devices[std::size_t(d)].bytes += double(numel(n.shape)) * 4;
        r.devices[std::size_t(d)].nodes++;
    }
    return r;
}

// Quantised weights: matmul_t reads encoded rows in place (decode one row into a per-thread buffer, then
// SIMD dot products against every input row), so a q4_k model costs its file size in memory, not 4 bytes/param.
namespace qweights {
using codecs::codec;
inline const codec* codec_of(const tensor_view& v) { return static_cast<const codec*>(v.qcodec); }
inline int fmt_of(const codec* c) {
    using simd::qfmt;
    return int(c->name == "q8_0" ? qfmt::q8_0 : c->name == "q4_k" ? qfmt::q4_k : c->name == "q6_k" ? qfmt::q6_k
             : c->name == "q5_k" ? qfmt::q5_k : qfmt::generic);
}
// Integer path (simd/qdot_i8.hpp): FORGE_CPU_INT8=0 turns it off, =1 forces it even when only the scalar
// reference kernels exist (it is on by default when the CPU has a SIMD integer table).
inline bool int8_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("FORGE_CPU_INT8");
        if (e && *e) return *e != '0';
        return std::string_view(simd::qi8_active().target) != "scalar";
    }();
    return on;
}
// y[m, n] = x[m, k] . W[n, k]^T for q8_0 / q4_k / q5_k / q6_k rows, on int8 activations. Each activation row is
// quantised once (on the pool when there are many); weights are read once per group of up to 8 activation rows.
// The result for one activation row does not depend on m (or on which thread computed which weight rows).
inline void gemm_nt_i8(int fmt, const std::uint8_t* w, std::size_t rb, std::size_t n, const float* x, std::size_t m,
                       std::size_t k, float* y) {
    const auto& qk = simd::qi8_active();
    const std::size_t ab = simd::qi8::act_bytes(fmt, k);
    thread_local std::vector<simd::qi8::act_k> act;   // reused across calls (caller thread); 32-byte aligned
    if (act.size() * sizeof(simd::qi8::act_k) < m * ab) act.resize((m * ab + sizeof(simd::qi8::act_k) - 1) / sizeof(simd::qi8::act_k));
    auto* a = reinterpret_cast<std::uint8_t*>(act.data());
    if (m * k <= 32768) for (std::size_t i = 0; i < m; ++i) qk.quantize(fmt, x + i * k, k, a + i * ab);
    else task::parallel_for(m, 1, [&](std::size_t b, std::size_t e) { for (std::size_t i = b; i < e; ++i) qk.quantize(fmt, x + i * k, k, a + i * ab); });
    if (m <= 8) {   // decode / small batches: stream the weights once, long claims first, short ones last
        task::parallel_for(n, 16, 256, [&](std::size_t b, std::size_t e) { qk.matvec(fmt, w + b * rb, rb, e - b, a, ab, k, m, y + b, n); });
    } else {        // prefill: 16-row blocks stay in L2 while every group of 8 activation rows passes over them
        task::parallel_for(n, 16, 16, [&](std::size_t b, std::size_t e) { qk.matvec(fmt, w + b * rb, rb, e - b, a, ab, k, m, y + b, n); });
    }
}
// Few input rows (decode): fused tile decode + dot per (row, weight row). Many rows (prefill): decode each weight
// row once (SIMD decoders) and reuse it. Both are bit-identical to dot(x, codec.decode(row)).
inline void gemm_nt(std::span<const tensor_view> in, tensor_view out, const ops::attrs&) {
    const auto& x = in[0]; const auto& w = in[1];
    std::size_t k = std::size_t(x.shape.back()), m = x.size() / k, n = std::size_t(w.shape[0]);
    const auto* c = codec_of(w); std::size_t rb = c->bytes(k);
    if (const int f = fmt_of(c); int8_enabled() && simd::qi8::supported(f, k)) return gemm_nt_i8(f, w.qdata, rb, n, x.data, m, k, out.data);
    // FORGE_QDOT=0: plain scalar codec decode + dot (the pre-SIMD path; benchmarking and bisecting)
    static const bool native = [] { auto* e = std::getenv("FORGE_QDOT"); return !(e && std::string(e) == "0"); }();
    int fmt = fmt_of(c);
    bool fused = native && m <= 2 && c->block <= 256 && 256 % c->block == 0;
    task::parallel_for(n, 8, [&](std::size_t b, std::size_t e) {
        const auto& ker = simd::active();
        if (fused) {
            for (std::size_t j = b; j < e; ++j)
                for (std::size_t i = 0; i < m; ++i)
                    out.data[i * n + j] = ker.qdot(fmt, w.qdata + j * rb, x.data + i * k, k, c->decode, c->block, c->block_bytes);
            return;
        }
        thread_local std::vector<float> row;
        if (row.size() < k) row.resize(k);
        for (std::size_t j = b; j < e; ++j) {
            if (native) ker.qdecode(fmt, w.qdata + j * rb, k, row.data(), c->decode); else c->decode(w.qdata + j * rb, k, row.data());
            for (std::size_t i = 0; i < m; ++i) out.data[i * n + j] = ker.dot(x.data + i * k, row.data(), k);
        }
    });
}
// gather (embedding lookup) straight from encoded rows: a q8_0/q4_k tok_emb never gets an f32 copy.
inline void gather_rows(std::span<const tensor_view> in, tensor_view out, const ops::attrs&) {
    const auto& w = in[0]; const auto* c = codec_of(w);
    std::size_t d = std::size_t(w.shape[1]), rowsn = std::size_t(w.shape[0]), rb = c->bytes(d);
    for (std::size_t i = 0; i < in[1].size(); ++i) {
        auto r = std::size_t(in[1].data[i]);
        if (r >= rowsn) raise(errc::invalid_argument, "gather index {} out of range", r);
        c->decode(w.qdata + r * rb, d, out.data + i * d);
    }
}
inline const lowering& gather_lowering() {
    static const lowering l{"cpu.qgather", ops::op::gather, 100, [](auto, auto&, auto&) { return true; }, gather_rows, 1};
    return l;
}
inline const lowering& gemm_nt_lowering() {
    static const lowering l{"cpu.qgemm_nt", ops::op::matmul_t, 100, [](auto, auto&, auto&) { return true; }, gemm_nt, 1};
    return l;
}
// ---- sparse mixture of experts (moe_ffn) ----
// Inputs: h [t, d], router logits [t, e], stacked expert weights gate/up [e, f, d] and down [e, d, f], each f32 or
// encoded (one expert = one contiguous slice of whole rows). Each token is routed to its top-k experts
// (ops::ref::moe_route); the (token, slot) pairs are grouped by expert, so each selected expert's weight rows are read
// once per layer however many tokens chose it, and only selected experts are touched. Work items are (expert, block of
// output columns) across all active experts in one parallel_for per projection; the rows use the same SIMD quantised
// dot / row-decode kernels as gemm_nt. Tokens are combined in slot order, so the result does not depend on threading.
struct moe_plan {
    i64 t = 0, k = 0, ne = 0;
    std::vector<i64> ids; std::vector<float> w;          // [t * k] routed expert and weight per (token, slot) pair
    std::vector<std::uint32_t> off, pair;                // expert e's rows are [off[e], off[e+1]); row -> pair index
    std::vector<std::uint32_t> row_of;                   // pair -> row
    void route(const float* logits, i64 t_, i64 ne_, const ops::attrs& a) {
        t = t_; ne = ne_; k = std::min<i64>(i64(a.num("k", 2)), ne);
        bool norm = a.num("norm", 1) != 0; double sc = a.num("scale", 1.0); int gating = int(a.num("gating", 0));
        ids.resize(std::size_t(t * k)); w.resize(std::size_t(t * k));
        for (i64 r = 0; r < t; ++r) ops::ref::moe_route(logits + r * ne, ne, k, norm, sc, gating, ids.data() + r * k, w.data() + r * k);
        off.assign(std::size_t(ne + 1), 0);
        for (auto e : ids) off[std::size_t(e) + 1]++;
        for (i64 e = 0; e < ne; ++e) off[std::size_t(e) + 1] += off[std::size_t(e)];
        pair.resize(ids.size()); row_of.resize(ids.size());
        std::vector<std::uint32_t> fill(off.begin(), off.end() - 1);
        for (std::size_t p = 0; p < ids.size(); ++p) { auto r = fill[std::size_t(ids[p])]++; pair[r] = std::uint32_t(p); row_of[p] = r; }
    }
};
// rows [r0, r1) of x (row r at xrow(r)) dotted with weight rows [c0, c1) of one expert -> y[r * ldy + c]
template <class XRow>
inline void moe_rows(const tensor_view& w, std::size_t e, std::size_t c0, std::size_t c1, std::size_t kd, std::size_t r0, std::size_t r1,
                     XRow xrow, float* y, std::size_t ldy, std::vector<float>& tmp) {
    const auto& ker = simd::active();
    std::size_t nrow = std::size_t(w.shape[1]);
    if (!w.qdata) {
        const float* W = w.data + (e * nrow) * kd;
        for (std::size_t c = c0; c < c1; ++c) for (std::size_t r = r0; r < r1; ++r) y[r * ldy + c] = ker.dot(xrow(r), W + c * kd, kd);
        return;
    }
    static const bool native = [] { auto* ev = std::getenv("FORGE_QDOT"); return !(ev && std::string(ev) == "0"); }();
    const auto* cd = codec_of(w); std::size_t rb = cd->bytes(kd); int fmt = fmt_of(cd);
    const std::uint8_t* W = w.qdata + e * nrow * rb;
    bool fused = native && r1 - r0 <= 2 && cd->block <= 256 && 256 % cd->block == 0;
    if (tmp.size() < kd) tmp.resize(kd);
    for (std::size_t c = c0; c < c1; ++c) {
        if (fused) { for (std::size_t r = r0; r < r1; ++r) y[r * ldy + c] = ker.qdot(fmt, W + c * rb, xrow(r), kd, cd->decode, cd->block, cd->block_bytes); continue; }
        if (native) ker.qdecode(fmt, W + c * rb, kd, tmp.data(), cd->decode); else cd->decode(W + c * rb, kd, tmp.data());
        for (std::size_t r = r0; r < r1; ++r) y[r * ldy + c] = ker.dot(xrow(r), tmp.data(), kd);
    }
}
inline void moe_ffn(std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) {
    const auto& h = in[0]; const auto& lg = in[1]; const auto& wg = in[2]; const auto& wu = in[3]; const auto& wd = in[4];
    std::size_t t = std::size_t(h.shape[0]), d = std::size_t(h.shape[1]), ne = std::size_t(wg.shape[0]), f = std::size_t(wg.shape[1]);
    // per-thread scratch, named through references: worker lambdas must see the calling thread's buffers
    static thread_local moe_plan mp_tls; static thread_local std::vector<float> g_tls, u_tls, d_tls;
    auto& mp = mp_tls; auto& g = g_tls; auto& u = u_tls; auto& dn = d_tls;
    mp.route(lg.data, i64(t), i64(ne), a);
    std::size_t rows = mp.ids.size();
    g.resize(rows * f); u.resize(rows * f); dn.resize(rows * d);
    // work items: (active expert, block of 16 output columns)
    std::vector<std::uint32_t> act; for (std::size_t e = 0; e < ne; ++e) if (mp.off[e + 1] > mp.off[e]) act.push_back(std::uint32_t(e));
    auto run = [&](std::size_t ncol, auto&& body) {
        std::size_t nb = (ncol + 15) / 16;
        task::parallel_for(act.size() * nb, 1, [&](std::size_t b, std::size_t en) {
            thread_local std::vector<float> tmp;
            for (std::size_t i = b; i < en; ++i) { std::size_t e = act[i / nb], c0 = (i % nb) * 16; body(e, c0, std::min(ncol, c0 + 16), tmp); }
        });
    };
    auto tok_row = [&](std::size_t r) { return h.data + std::size_t(mp.pair[r] / std::uint32_t(mp.k)) * d; };
    run(f, [&](std::size_t e, std::size_t c0, std::size_t c1, std::vector<float>& tmp) {
        moe_rows(wg, e, c0, c1, d, mp.off[e], mp.off[e + 1], tok_row, g.data(), f, tmp);
        moe_rows(wu, e, c0, c1, d, mp.off[e], mp.off[e + 1], tok_row, u.data(), f, tmp);
    });
    simd::active().silu_mul(g.data(), u.data(), g.data(), rows * f);
    run(d, [&](std::size_t e, std::size_t c0, std::size_t c1, std::vector<float>& tmp) {
        moe_rows(wd, e, c0, c1, f, mp.off[e], mp.off[e + 1], [&](std::size_t r) { return g.data() + r * f; }, dn.data(), d, tmp);
    });
    for (std::size_t r = 0; r < t; ++r) {   // combine in slot order
        float* o = out.data + r * d;
        std::fill(o, o + d, 0.f);
        for (std::size_t j = 0; j < std::size_t(mp.k); ++j) { std::size_t p = r * std::size_t(mp.k) + j;
            simd::active().saxpy(mp.w[p], dn.data() + std::size_t(mp.row_of[p]) * d, o, d); }
    }
}
inline const lowering& moe_lowering() {
    static const lowering l{"cpu.moe_ffn", ops::op::moe_ffn, 100, [](auto, auto&, auto&) { return true; }, moe_ffn, 1};
    return l;
}
// f32 copies for quantised weights that something other than matmul_t consumes, shared across plans.
inline std::shared_ptr<tensor> decoded(const std::uint8_t* p, const codec* c, const shape_t& s) {
    static std::mutex mu; static std::map<const std::uint8_t*, std::weak_ptr<tensor>> cache;
    std::lock_guard lk(mu);
    if (auto sp = cache[p].lock()) return sp;
    auto t = std::make_shared<tensor>(s);
    c->decode(p, t->size(), t->data.data());
    cache[p] = t;
    return t;
}
} // namespace qweights

struct step {
    const lowering* low;
    std::vector<value_id> in;
    value_id out;
    const ops::attrs* attrs;
    int kind; // 0 op, 1 state_append, 2 state_op
    // device runs: this step and the next may run as one kernel (device_hooks::fused): 1 "add_rms_norm", 2 "rms_rope";
    // fattrs are the fused kernel's attributes (see fuse_steps)
    int fuse = 0;
    const ops::attrs* fattrs = nullptr;
};

class plan {
public:
    tensor_graph g;
    memory_plan mem;
    std::vector<step> steps;
    std::deque<ops::attrs> attr_store;
    std::vector<std::shared_ptr<tensor>> decoded_weights;   // f32 fallbacks for quantised weights   // per-step attribute overrides (stable addresses)
    std::vector<tensor_view> views;   // per value
    std::vector<std::pair<value_id, value_id>> state_aliases;  // (view, state node) pairs rebound at run time
    std::vector<float> arena;
    std::map<std::string, value_id> inputs;
    partition_report part;
    std::vector<int> stage_of_node;    // pipeline stage per node (empty = single device)
    device_hooks* hooks = nullptr;     // device residency protocol (null = everything on the host)
    std::shared_ptr<device_hooks> hooks_ref;   // keeps the hooks (and the runtime behind them) alive for ~plan
    bool arena_resident = false;       // arena held on the device across runs (device_hooks::arena)
    std::size_t replays = 0;           // device_hooks::replay runs since the last real run
    plan() = default;
    plan(const plan&) = delete;
    plan& operator=(const plan&) = delete;
    ~plan() {
        if (!hooks_ref || !hooks_ref->forget) return;
        try {
            if (arena_resident) hooks_ref->forget(arena.data(), arena.size());
            // constants pinned by run() live in this plan's graph: drop their device copies (and pin records) before
            // the host memory goes, or a later buffer at the same address would find a stale resident entry
            if (pinned) for (std::size_t i = 0; i < g.size() && i < views.size(); ++i)
                if (g.nodes()[i].kind == node_kind::constant && views[i].data) hooks_ref->forget(views[i].data, views[i].size());
        } catch (...) {}
    }
    bool pinned = false;
    bool deterministic = true;        // CPU kernels use fixed reduction orders
    std::string describe() const {
        std::string s = std::format("plan: {} nodes, {} steps, arena {} bytes (naive {}), {} in-place, {} aliases\n",
                                    g.size(), steps.size(), mem.arena_bytes, mem.naive_bytes, mem.in_place, mem.aliases);
        std::map<std::string, int> uses;
        for (auto& st : steps) if (st.low) uses[st.low->name]++;
        for (auto& [k, v] : uses) s += std::format("  {:<16} x{}\n", k, v);
        return s;
    }
};

inline void fuse_steps(plan& p);
// FFN gate/up fusion ($FORGE_FUSE_GLU=0 off): silu_mul(matmul_t(x, Wg), matmul_t(x, Wu)) whose two device quantised
// matmuls are the two steps right before it (so x is still live there), read the same x, have weights of one codec
// (registry::device_qglu_codecs) and shape, and feed nothing else, becomes one registry::device_qglu step with inputs
// (x, Wg, Wu, gate, up). The gate/up buffers stay planned: the step's fallback writes them as the unfused steps did.
inline void fuse_glu(plan& p, registry& reg) {
    if (!reg.device_qglu || !reg.device_qgemm || !p.hooks || !reg.stages.empty()) return;
    if (auto* e = std::getenv("FORGE_FUSE_GLU"); e && std::string(e) == "0") return;
    auto& G = p.g;
    std::vector<int> uses(G.size(), 0);
    for (auto& n : G.nodes()) for (auto in : n.inputs) ++uses[in];
    for (auto& [nm, v] : G.outputs()) uses[v] += 2;
    const lowering* qg = &*reg.device_qgemm;
    for (std::size_t i = 2; i < p.steps.size(); ++i) {
        const step s = p.steps[i], a = p.steps[i - 2], b = p.steps[i - 1];
        if (s.kind || a.kind || b.kind || a.low != qg || b.low != qg || s.in.size() != 2 || G.at(s.out).kind != node_kind::op || G.at(s.out).op != ops::op::silu_mul) continue;
        const step* gs = a.out == s.in[0] ? &a : b.out == s.in[0] ? &b : nullptr;
        const step* us = a.out == s.in[1] ? &a : b.out == s.in[1] ? &b : nullptr;
        if (!gs || !us || gs == us || gs->in.size() != 2 || us->in.size() != 2 || gs->in[0] != us->in[0]) continue;
        const auto &wg = G.at(gs->in[1]), &wu = G.at(us->in[1]);
        if (wg.qcodec != wu.qcodec || wg.shape != wu.shape || wg.shape.size() != 2 || !p.views[gs->in[1]].qdata || !p.views[us->in[1]].qdata ||
            std::find(reg.device_qglu_codecs.begin(), reg.device_qglu_codecs.end(), wg.qcodec) == reg.device_qglu_codecs.end()) continue;
        if (uses[s.in[0]] != 1 || uses[s.in[1]] != 1 || G.at(s.in[0]).shape != G.at(s.out).shape || G.at(s.in[1]).shape != G.at(s.out).shape) continue;
        p.steps[i - 2] = step{&*reg.device_qglu, {gs->in[0], gs->in[1], us->in[1], s.in[0], s.in[1]}, s.out, s.attrs, 0};
        // the steps now at i - 1 and i have the fused step among their two predecessors, so the scan goes on at i + 1
        p.steps.erase(p.steps.begin() + std::ptrdiff_t(i) - 1, p.steps.begin() + std::ptrdiff_t(i) + 1);
    }
}

inline std::unique_ptr<plan> compile(tensor_graph g, state_store* st = nullptr, const pass_options& po = {}) {
    auto p = std::make_unique<plan>();
    auto sshapes = st ? st->shapes() : std::map<std::string, shape_t>{};
    p->g = optimise(std::move(g), po, sshapes);
    p->part = partition(p->g, gpu::all());
    p->mem = plan_memory(p->g);
    if (!plan_is_safe(p->mem)) raise(errc::invalid_argument, "memory planner produced overlapping buffers");
    p->arena.assign(p->mem.arena_bytes / sizeof(float) + 1, 0.f);
    auto& G = p->g;
    p->views.resize(G.size());
    auto& reg = po.lowerings ? *po.lowerings : registry::cpu();
    p->hooks = reg.stages.empty() ? reg.hooks.get() : nullptr;
    if (p->hooks) p->hooks_ref = reg.hooks;
    // Pipeline split: contiguous node ranges per stage, balanced by spec cost x output size.
    std::vector<registry*> stage_of(G.size(), &reg);
    if (!reg.stages.empty()) {
        std::vector<double> c(G.size(), 0.0); double tot = 0;
        for (std::size_t i = 0; i < G.size(); ++i) { auto& n = G.nodes()[i]; if (n.kind != node_kind::op) continue;
            double e = double(std::max<i64>(numel(n.shape), 1)); for (auto in : n.inputs) e = std::max(e, double(numel(G.at(in).shape)));
            c[i] = std::max(ops::info(n.op).cost, 1.0) * e; tot += c[i]; }
        auto k = reg.stages.size(); std::vector<double> w(reg.stage_weight);
        w.resize(k, 1.0); double ws = 0; for (auto x : w) ws += x;
        double acc = 0, edge = w[0] / ws * tot; std::size_t st = 0;
        for (std::size_t i = 0; i < G.size(); ++i) {
            while (st + 1 < k && acc >= edge) { ++st; edge += w[st] / ws * tot; }
            auto* r = reg.stages[st].get(); stage_of[i] = r ? r : &registry::cpu(); acc += c[i];
        }
        p->stage_of_node.resize(G.size()); for (std::size_t i = 0; i < G.size(); ++i) {
            std::size_t s = 0; for (; s < k; ++s) if ((reg.stages[s] ? reg.stages[s].get() : &registry::cpu()) == stage_of[i]) break;
            p->stage_of_node[i] = int(s); }
    }
    std::vector<bool> q_ok(G.size(), true);   // every consumer is matmul_t (weight operand), gather (table) or moe_ffn (experts)
    std::vector<bool> moe_w(G.size(), false);  // stacked expert weights [e, rows, k] of a moe_ffn
    for (auto& n : G.nodes()) for (std::size_t j = 0; j < n.inputs.size(); ++j) {
        bool moe = n.kind == node_kind::op && n.op == ops::op::moe_ffn && j >= 2;
        if (moe) moe_w[n.inputs[j]] = true;
        if (!(n.kind == node_kind::op && ((n.op == ops::op::matmul_t && j == 1) || (n.op == ops::op::gather && j == 0) || moe)))
            q_ok[n.inputs[j]] = false;
    }
    for (auto& [nm, v] : G.outputs()) q_ok[v] = false;
    bool q_direct = !p->hooks && reg.stages.empty();
    // Resident device backends have no quantised kernels, so a weight they consume is decoded to an f32 host copy
    // and mirrored on the device (4 bytes/param on both sides: a 9B q4_k model needed ~36 GB of RAM). Decode only up
    // to FORGE_GPU_WEIGHT_BUDGET bytes of f32 (default 8G; accepts K/M/G suffixes); the remaining matmul_t/gather
    // weights stay encoded in the mapped file and run on the CPU quantised kernels. Backends with a device quantised
    // matmul (registry::device_qgemm) keep those codecs encoded and run them on the device instead.
    bool q_spill = p->hooks && reg.stages.empty();
    std::size_t q_budget = std::size_t(8) << 30, q_used = 0;
    auto dev_q = [&](const std::string& codec) {
        return p->hooks && reg.device_qgemm && std::find(reg.device_qgemm_codecs.begin(), reg.device_qgemm_codecs.end(), codec) != reg.device_qgemm_codecs.end(); };
    if (auto* e = std::getenv("FORGE_GPU_WEIGHT_BUDGET"); e && *e) {
        char* end = nullptr; double v = std::strtod(e, &end);
        double mul = 1; if (end) switch (*end) { case 'k': case 'K': mul = 1024.0; break; case 'm': case 'M': mul = 1024.0 * 1024; break;
                                                  case 'g': case 'G': mul = 1024.0 * 1024 * 1024; break; default: break; }
        q_budget = v > 0 ? std::size_t(v * mul) : 0;
    }
    // state slots read as attention values (input 2): they take the store's value codec in mixed modes (k8v4)
    std::set<value_id> value_slots;
    for (std::size_t i = 0; i < G.size(); ++i) {
        auto& n = G.nodes()[i];
        if (n.kind == node_kind::op && n.op == ops::op::attention && n.inputs.size() == 3 && G.at(n.inputs[2]).kind == node_kind::state_append)
            value_slots.insert(n.inputs[2]);
    }
    for (std::size_t i = 0; i < G.size(); ++i) {
        auto& n = G.nodes()[i];
        auto& s = p->mem.slots[i];
        tensor_view v; v.shape = n.shape;
        if (n.kind == node_kind::weight && n.qweight) {
            auto* c = codecs::find(n.qcodec);
            if (!c) raise(errc::unsupported, "weight {}: unknown codec {}", n.name, n.qcodec);
            bool direct_ok = q_ok[i] && n.shape.size() == (moe_w[i] ? 3u : 2u) && std::size_t(n.shape.back()) % c->block == 0;
            std::size_t f32_bytes = std::size_t(numel(n.shape)) * sizeof(float);
            // expert weights stay encoded on every path: the CPU and device moe_ffn kernels read them in place
            bool keep_q = direct_ok && (moe_w[i] || q_direct || dev_q(n.qcodec) || (q_spill && q_used + f32_bytes > q_budget));
            if (direct_ok && q_spill && !keep_q) q_used += f32_bytes;
            if (keep_q) { v.qdata = n.qweight; v.qcodec = c; }
            else { auto t = qweights::decoded(n.qweight, c, n.shape); v.data = t->data.data(); p->decoded_weights.push_back(std::move(t)); }
        }
        else if (n.kind == node_kind::weight) v.data = const_cast<float*>(n.weight);
        else if (n.kind == node_kind::constant) v.data = n.constant->data.data();
        else if (n.kind == node_kind::state_append) {
            if (!st) raise(errc::invalid_argument, "graph uses state slot '{}' but no state store given", n.name);
            auto& sl = st->slots[n.name];
            const auto& xs = G.at(n.inputs[0]).shape;
            if (n.shape[0] > st->capacity) raise(errc::out_of_memory, "state slot '{}' exceeds capacity {}", n.name, st->capacity);
            if (!sl.capacity) sl.codec = st->codec_for(xs, value_slots.count(value_id(i)) != 0);
            auto ps = state_store::physical(xs, sl.codec);
            st->ensure(sl, ps, std::max<i64>(n.shape[0], 1));
            v.shape = state_store::physical(n.shape, sl.codec);
            v.data = sl.buf.data.data();   // rebound on every run (pages may move, plans may be shared)
            p->steps.push_back({nullptr, n.inputs, value_id(i), &n.attrs, 1});
        } else {
            value_id r = value_id(i);
            while (p->mem.slots[r].kind == buffer_slot::alias) r = p->mem.slots[r].root;
            const auto& rs = p->mem.slots[r];
            if (rs.kind == buffer_slot::external) {
                v.data = p->views[r].data;
                if (G.at(r).kind == node_kind::state_append) p->state_aliases.push_back({value_id(i), r});
            }
            else v.data = p->arena.data() + rs.offset / sizeof(float);
        }
        (void)s;
        p->views[i] = v;
        if (n.kind == node_kind::input) p->inputs[n.name] = value_id(i);
        if (n.kind == node_kind::state_op) {
            if (!st) raise(errc::invalid_argument, "graph uses state slot '{}' but no state store given", n.name);
            for (auto in : n.inputs)
                if (G.at(in).kind == node_kind::state_append)
                    raise(errc::unsupported, "state slot '{}' feeds state_op '{}'", G.at(in).name, n.name);
            p->steps.push_back({nullptr, n.inputs, value_id(i), &n.attrs, 2});
        }
        if (n.kind == node_kind::op) {
            // state views grow at run time; only consumers that read the live length are allowed
            for (auto in : n.inputs)
                if (G.at(in).kind == node_kind::state_append && n.op != ops::op::attention)
                    raise(errc::unsupported, "state slot '{}' feeds '{}': only attention may consume a growing state view",
                          G.at(in).name, ops::info(n.op).name);
            std::vector<shape_t> ins; for (auto in : n.inputs) ins.push_back(G.at(in).shape);
            const ops::attrs* at = &n.attrs;
            if (n.op == ops::op::attention && st && n.inputs.size() == 3) {
                auto codec_of = [&](value_id in) {
                    auto& m = G.at(in);
                    return m.kind == node_kind::state_append ? st->slots[m.name].codec : kv_codec::f32;
                };
                auto kc = codec_of(n.inputs[1]), vc = codec_of(n.inputs[2]);
                if (kc != kv_codec::f32 || vc != kv_codec::f32) {
                    auto& a2 = p->attr_store.emplace_back(n.attrs);
                    a2.v["kv_codec_k"] = double(int(kc)); a2.v["kv_codec_v"] = double(int(vc));
                    a2.v["kv_dv"] = double(G.at(n.inputs[2]).shape[2]);
                    at = &a2;
                }
            }
            if (n.op == ops::op::moe_ffn && n.inputs.size() == 5) {
                bool dev = p->hooks && reg.device_moe;
                for (std::size_t j = 2; j < 5; ++j) { auto& wn = G.at(n.inputs[j]);
                    dev = dev && p->views[n.inputs[j]].qdata && std::find(reg.device_moe_codecs.begin(), reg.device_moe_codecs.end(), wn.qcodec) != reg.device_moe_codecs.end(); }
                p->steps.push_back({dev ? &*reg.device_moe : &qweights::moe_lowering(), n.inputs, value_id(i), at, 0});
            }
            else if (n.op == ops::op::matmul_t && n.inputs.size() == 2 && p->views[n.inputs[1]].qdata)
                p->steps.push_back({dev_q(G.at(n.inputs[1]).qcodec) ? &*reg.device_qgemm : &qweights::gemm_nt_lowering(), n.inputs, value_id(i), at, 0});
            else if (n.op == ops::op::gather && n.inputs.size() == 2 && p->views[n.inputs[0]].qdata)
                p->steps.push_back({dev_q(G.at(n.inputs[0]).qcodec) && reg.device_qgather && G.at(n.inputs[0]).shape.size() == 2 && G.at(n.inputs[0]).shape[1] % 32 == 0
                                    ? &*reg.device_qgather : &qweights::gather_lowering(), n.inputs, value_id(i), at, 0});
            else p->steps.push_back({&stage_of[i]->select(n.op, ins, n.shape, n.attrs), n.inputs, value_id(i), at, 0});
        }
    }
    fuse_glu(*p, reg);   // first: it removes steps; fuse_steps then pairs adjacent steps of the final list
    fuse_steps(*p);
    return p;
}

// Elementwise fusions for device runs: mark adjacent device step pairs that the backend may run as one kernel
// (device_hooks::fused); the executor falls back to the two steps whenever the hook declines. The memory plan is
// untouched, so the unfused path always has its buffers.
//   add -> rms_norm of the sum (residual add + the next norm)                 fuse 1, "add_rms_norm"
//   rms_norm -> rope of the norm, when the rope is its only consumer (QK-norm)  fuse 2, "rms_rope"
inline void fuse_steps(plan& p) {
    if (!p.hooks || !p.hooks->fused) return;
    auto& G = p.g;
    std::vector<int> uses(G.size(), 0);
    for (auto& n : G.nodes()) for (auto in : n.inputs) ++uses[in];
    for (auto& [nm, v] : G.outputs()) uses[v] += 2;
    auto dev_op = [&](const step& s, ops::op o) { return s.kind == 0 && s.low && s.low->on_device && G.at(s.out).kind == node_kind::op && G.at(s.out).op == o; };
    for (std::size_t i = 0; i + 1 < p.steps.size(); ++i) {
        auto& a = p.steps[i]; auto& b = p.steps[i + 1];
        if (b.in.size() != 2 || a.in.size() != 2 || b.in[0] != a.out) continue;
        const i64 n = numel(G.at(a.out).shape);
        if (dev_op(a, ops::op::add) && dev_op(b, ops::op::rms_norm) && numel(G.at(a.in[0]).shape) == n && numel(G.at(a.in[1]).shape) == n) {
            a.fuse = 1; a.fattrs = b.attrs; ++i;
        } else if (dev_op(a, ops::op::rms_norm) && dev_op(b, ops::op::rope) && uses[a.out] == 1) {
            auto& at = p.attr_store.emplace_back(*b.attrs); at.v["eps"] = a.attrs->num("eps", 1e-6);
            a.fuse = 2; a.fattrs = &at; ++i;
        }
    }
}

class executor {
public:
    explicit executor(plan& p, state_store* st = nullptr) : p_(p), st_(st) {}
    void set_input(const std::string& name, std::span<const float> data) {
        auto it = p_.inputs.find(name);
        if (it == p_.inputs.end()) return; // input eliminated by DCE
        auto& v = p_.views[it->second];
        if (data.size() != v.size()) raise(errc::shape_mismatch, "input {} expects {} elements, got {}", name, v.size(), data.size());
        auto* h = p_.hooks;   // the whole view is overwritten: no need to fetch the device copy first
        std::copy(data.begin(), data.end(), v.data);
        if (h) h->host_wrote(v.data, v.size());
    }
    void run() {
        auto* h = p_.hooks;
        if (!h) return run_steps(nullptr);
        if (!p_.pinned) {   // weights and constants never change: they stay on the device across runs
            for (std::size_t i = 0; i < p_.g.size(); ++i) {
                auto k = p_.g.nodes()[i].kind;
                if ((k == node_kind::weight || k == node_kind::constant) && p_.views[i].data) h->pin(p_.views[i].data, p_.views[i].size());
            }
            p_.pinned = true;
        }
        h->begin_run();
        // a resident arena (up to $FORGE_GPU_ARENA_MB, default 1024; 0 = off): fixed device addresses for every
        // intermediate, no per-run allocations or re-splits
        static const std::size_t arena_cap = [] { auto* e = std::getenv("FORGE_GPU_ARENA_MB"); return std::size_t(e ? std::strtoull(e, nullptr, 10) : 1024) << 20; }();
        if (h->arena && h->forget && p_.arena.size() * sizeof(float) <= arena_cap) {
            try { h->arena(p_.arena.data(), p_.arena.size()); p_.arena_resident = true; } catch (...) { h->end_run(); throw; }
        }
        const void* key = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&p_) ^ (reinterpret_cast<std::uintptr_t>(st_) * 31));
        bool replayed = false;
        try {
            if (h->replay && p_.arena_resident && h->end_run_discard && replayable()) {
                static const std::size_t verify = [] { auto* e = std::getenv("FORGE_REPLAY_VERIFY"); auto v = e ? std::strtoull(e, nullptr, 10) : 32ull; return std::size_t(v ? v : 1); }();
                if (p_.replays + 1 < verify && h->replay(key)) { replayed = true; ++p_.replays; advance_state(); }
            }
            if (!replayed) {
                p_.replays = 0; host_ops_ = false;
                run_steps(h);
                if (h->ran) h->ran(key, !host_ops_);
            }
        } catch (...) { h->end_run(); throw; }
        argmax_ = -1;
        if (!h->end_run_discard) return h->end_run();
        // only outputs and persistent state outlive the run: fetch those, drop the device intermediates unread
        try {
            // greedy decode: the first output's argmax on the device replaces fetching that output (see want_argmax)
            if (want_argmax_ && h->argmax && !p_.g.outputs().empty()) {
                auto v = p_.g.outputs().front().second;
                if (p_.views[v].data) argmax_ = h->argmax(p_.views[v].data, p_.views[v].size());
            }
            for (auto& [nm, v] : p_.g.outputs()) if (p_.views[v].data && !(argmax_ >= 0 && v == p_.g.outputs().front().second)) h->to_host(p_.views[v].data, p_.views[v].size());
            if (st_) for (auto& [k, sl] : st_->slots) if (!sl.buf.data.empty()) {
                if (h->keep) { st_->hooks = h; h->keep(sl.buf.data.data(), sl.buf.data.size()); }
                else h->to_host(sl.buf.data.data(), sl.buf.data.size());
            }
        } catch (...) { h->end_run(); throw; }
        h->end_run_discard();
    }
private:
    bool host_ops_ = false;   // a step of this run went through the host
    // A replayed run repeats the device work of the previous one; the host bookkeeping it would have done is only
    // the state slot lengths. Not when a slot would grow or move, holds a host-encoded codec, or offloads.
    bool replayable() const {
        if (!st_ || st_->offload || st_->snap_row >= 0) return false;
        for (auto& s : p_.steps) {
            if (s.kind == 0) continue;
            auto it = st_->slots.find(p_.g.at(s.out).name);
            if (it == st_->slots.end() || it->second.len == 0) return false;
            if (s.kind == 1 && (it->second.codec != kv_codec::f32 || it->second.len + p_.views[s.in[0]].shape[0] > it->second.capacity)) return false;
        }
        return true;
    }
    void advance_state() {
        for (auto& s : p_.steps) {
            if (s.kind == 0) continue;
            auto& sl = st_->slots[p_.g.at(s.out).name];
            sl.len += p_.views[s.in[0]].shape[0];
            if (s.kind == 1) {
                auto& out = p_.views[s.out];
                out.data = sl.buf.data.data();
                for (auto [a, r] : p_.state_aliases) if (r == s.out) p_.views[a].data = out.data;
                out.shape[0] = sl.len;
            }
        }
    }
    // FORGE_OP_TIME=1: wall time per op kind (state ops by fn, attention split by KV codec), printed at exit
    struct op_time {
        static bool on() { static const bool b = [] { const char* e = std::getenv("FORGE_OP_TIME"); return e && *e && *e != '0'; }(); return b; }
        static void add(const std::string& k, double sec) {
            static std::mutex m; static std::map<std::string, std::pair<double, std::size_t>>* t = nullptr;
            std::lock_guard g(m);
            if (!t) {
                t = new std::map<std::string, std::pair<double, std::size_t>>();
                std::atexit([] {
                    double tot = 0; for (auto& [n, v] : *t) tot += v.first;
                    std::vector<std::pair<double, std::string>> r; for (auto& [n, v] : *t) r.push_back({v.first, n});
                    std::sort(r.rbegin(), r.rend());
                    std::fprintf(stderr, "op time (FORGE_OP_TIME), total %.3f s\n", tot);
                    for (auto& [sec, n] : r) std::fprintf(stderr, "  %-28s %9.3f s %5.1f%%  %zu calls\n", n.c_str(), sec, 100 * sec / std::max(tot, 1e-12), (*t)[n].second);
                });
            }
            auto& e = (*t)[k]; e.first += sec; ++e.second;
        }
        std::string key; std::chrono::steady_clock::time_point t0;
        explicit op_time(std::string k) : key(std::move(k)), t0(std::chrono::steady_clock::now()) {}
        ~op_time() { add(key, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()); }
    };
    std::string op_key(const step& s) const {
        if (s.kind == 1) return "state_append";
        if (s.kind == 2) return "state." + s.attrs->str("fn");
        std::string k(ops::info(p_.g.at(s.out).op).name);
        if (p_.g.at(s.out).op == ops::op::attention) k += s.attrs->num("kv_codec_k", 0) != 0 ? ".q" : ".f32";
        return k;
    }
    // one fixed-size state op over the rows of `ins`: on the device when the backend has the kernel, else on the host
    void run_state_op(device_hooks* h, const std::string& fn, std::span<const tensor_view> ins, tensor_view out, state_store::slot& sl, const ops::attrs& at) {
        if (h && h->state && h->state(fn, ins, sl.buf.data.data(), sl.buf.data.size(), out, at)) { sl.len += ins[0].shape[0]; return; }
        if (h) { for (auto& v : ins) h->to_host(v.data, v.size()); h->to_host(out.data, out.size()); h->to_host(sl.buf.data.data(), sl.buf.data.size()); }
        host_ops_ = true;
        if (fn == "causal_conv")
            state_ops::causal_conv(ins[0].data, ins[1].data, sl.buf.data.data(), out.data, ins[0].shape[0], ins[0].shape[1], ins[1].shape[1]);
        else if (fn == "delta_rule") {
            i64 dk = ins[0].shape[2];
            state_ops::delta_rule(ins[0].data, ins[1].data, ins[2].data, ins[3].data, ins[4].data, sl.buf.data.data(), out.data,
                                  ins[0].shape[0], ins[0].shape[1], dk, ins[2].shape[1], ins[2].shape[2],
                                  at.num("l2norm", 1) != 0, float(at.num("scale", 1.0 / std::sqrt(double(dk)))));
        } else raise(errc::not_found, "unknown state op '{}'", fn);
        sl.len += ins[0].shape[0];
        if (h) { h->host_wrote(out.data, out.size()); h->host_wrote(sl.buf.data.data(), sl.buf.data.size()); }
    }
    void run_steps(device_hooks* h) {
        std::vector<tensor_view> ins;
        for (std::size_t si = 0; si < p_.steps.size(); ++si) {
            auto& s = p_.steps[si];
            std::optional<op_time> timer;
            if (op_time::on()) timer.emplace(op_key(s));
            ins.clear();
            for (auto v : s.in) ins.push_back(p_.views[v]);
            auto& out = p_.views[s.out];
            if (s.fuse && h && h->fused && si + 1 < p_.steps.size()) {   // this step and the next as one kernel (fuse_steps)
                auto& nx = p_.steps[si + 1];
                const tensor_view fin[] = {ins[0], ins[1], p_.views[nx.in[1]]}, fo2[] = {out, p_.views[nx.out]};
                bool done = s.fuse == 1 ? h->fused("add_rms_norm", fin, fo2, *s.fattrs)
                                        : h->fused("rms_rope", fin, std::span<const tensor_view>(fo2 + 1, 1), *s.fattrs);
                if (done) { ++si; continue; }
            }
            if (s.kind == 2) {   // fixed-size state op: state is host memory, updated in place
                auto& node = p_.g.at(s.out);
                auto fn = s.attrs->str("fn");
                std::vector<shape_t> shs; for (auto& v : ins) shs.push_back(v.shape);
                auto ss = state_ops::state_shape(fn, shs);
                auto& sl = st_->slots[node.name];
                if (sl.len == 0 || sl.buf.shape != ss) {   // fresh: zeros (in place when the shape is unchanged, so a device copy stays keyed)
                    host_ops_ = true;
                    if (sl.buf.shape == ss && !sl.buf.data.empty()) { std::fill(sl.buf.data.begin(), sl.buf.data.end(), 0.f); if (h) h->host_wrote(sl.buf.data.data(), sl.buf.data.size()); }
                    else { if (h && !sl.buf.data.empty()) h->release(sl.buf.data.data(), sl.buf.data.size()); sl.buf = tensor(ss); }
                    sl.len = 0; sl.fixed = true;
                }
                // speculative snapshot (state_store::snap_row): run rows [0, cut), copy the state aside, run the rest
                if (i64 rows = ins[0].shape[0], cut = st_->snap_row; cut >= 0) {
                    auto sub = [&](i64 a, i64 b) {
                        std::vector<tensor_view> si = ins; tensor_view so = out;
                        auto rows_of = [&](tensor_view& v) { std::size_t w = v.size() / std::size_t(rows); v.data += std::size_t(a) * w; v.shape[0] = b - a; };
                        for (std::size_t i = 0; i < si.size(); ++i) if (!(fn == "causal_conv" && i == 1)) rows_of(si[i]);
                        rows_of(so);
                        run_state_op(h, fn, si, so, sl, *s.attrs);
                    };
                    cut = std::min(cut, rows);
                    if (cut > 0) sub(0, cut);
                    st_->take_snapshot(node.name, sl.buf.data.data(), sl.buf.data.size(), sl.len, h);
                    if (cut < rows) sub(cut, rows);
                    continue;
                }
                run_state_op(h, fn, ins, out, sl, *s.attrs);
                continue;
            }
            if (s.kind == 1) {
                auto& node = p_.g.at(s.out);
                auto& sl = st_->slots[node.name];
                i64 rows = ins[0].shape[0];
                auto ps = state_store::physical(ins[0].shape, sl.codec);
                i64 row = numel(ps) / std::max<i64>(rows, 1);
                if (sl.len + rows > sl.capacity) host_ops_ = true;   // the slot grows: its buffer moves
                if (h && sl.capacity && sl.len + rows > sl.capacity) h->release(sl.buf.data.data(), sl.buf.data.size());  // about to move: sync + drop
                const float* before = sl.buf.data.data();
                st_->ensure(sl, ps, sl.len + rows);
                float* dst = sl.buf.data.data() + sl.len * row;
                std::size_t nphys = std::size_t(rows * row);
                if (sl.codec != kv_codec::f32) {   // quantise on the host (the device copy of the rows is patched)
                host_ops_ = true;
                    if (h) { h->to_host(ins[0].data, ins[0].size()); h->to_host(dst, nphys); if (sl.buf.data.data() != before) h->host_wrote(sl.buf.data.data(), sl.buf.data.size()); }
                    i64 heads = ins[0].shape[1], d = ins[0].shape[2], hw = kv_head_words(d, sl.codec);
                    task::parallel_for(std::size_t(rows * heads), 64, [&](std::size_t b, std::size_t e) {   // on the pool: long prompts
                        for (std::size_t r = b; r < e; ++r) kv_encode_head(ins[0].data + i64(r) * d, d, sl.codec, dst + i64(r) * hw);
                    });
                    if (h) h->host_wrote(dst, nphys);
                } else if (h && h->copy) {
                    if (sl.buf.data.data() != before) h->host_wrote(sl.buf.data.data(), sl.buf.data.size());
                    h->copy(ins[0].data, ins[0].size(), sl.buf.data.data(), sl.buf.data.size(), std::size_t(sl.len * row));
                } else {
                    if (h) { h->to_host(ins[0].data, ins[0].size()); h->to_host(dst, ins[0].size()); }
                host_ops_ = true;
                    std::copy_n(ins[0].data, ins[0].size(), dst);
                    if (h) h->host_wrote(dst, ins[0].size());
                }
                sl.len += ins[0].shape[0];
                out.data = sl.buf.data.data();
                for (auto [a, r] : p_.state_aliases) if (r == s.out) p_.views[a].data = out.data;
                out.shape[0] = sl.len;   // symbolic length: downstream kernels read it at run time
                continue;
            }
            if (h && p_.g.at(s.out).op == ops::op::reshape && ins.size() == 1 && ins[0].size() == out.size() &&
                (ins[0].data == out.data || ins[0].data + ins[0].size() <= out.data || out.data + out.size() <= ins[0].data)) {
                if (ins[0].data != out.data) h->copy(const_cast<float*>(ins[0].data), out.size(), out.data, out.size(), 0);   // stays on the device (aliased: no-op)
                continue;
            }
            if (h && h->copy && p_.g.at(s.out).op == ops::op::concat && !ins.empty() && concat_on_device(*h, ins, out, *s.attrs)) continue;
            if (h && !s.low->on_device) {
                host_ops_ = true;
                if (std::getenv("FORGE_PLAN_TRACE")) {   // which ops round-trip through the host on a device run
                    static std::map<std::string, std::size_t> c; static bool reg = false;
                    if (!reg) { reg = true; std::atexit([] { for (auto& [k, n] : c) std::fprintf(stderr, "host-op %s %zu\n", k.c_str(), n); }); }
                    std::string key(ops::info(p_.g.at(s.out).op).name);
                    for (auto& v : ins) { key += " ["; for (auto d : v.shape) key += std::to_string(d) + ","; key += "]"; }
                    for (auto& [ak, av] : s.attrs->v) if (auto* d = std::get_if<double>(&av)) key += " " + ak + "=" + std::to_string(int(*d));
                    ++c[key];
                }
                for (auto& v : ins) if (v.data) h->to_host(v.data, v.size());   // encoded weights (qdata) live on the host only
                h->to_host(out.data, out.size());
                s.low->fn(ins, out, *s.attrs);
                h->host_wrote(out.data, out.size());
            } else s.low->fn(ins, out, *s.attrs);
            if (h && st_ && st_->offload && p_.g.at(s.out).op == ops::op::attention)
                for (auto v : s.in) if (p_.g.at(v).kind == node_kind::state_append) {
                    auto& sl = st_->slots[p_.g.at(v).name];
                    h->release(sl.buf.data.data(), sl.buf.data.size());
                }
        }
    }
    // concat as strided device-to-device copies (one per outer index and input); false = leave it to the host kernel
    static bool concat_on_device(device_hooks& h, const std::vector<tensor_view>& ins, tensor_view out, const ops::attrs& a) {
        auto r = i64(out.shape.size()); if (!r) return false;
        i64 ax = i64(a.num("axis", 0)); if (ax < 0) ax += r; if (ax < 0 || ax >= r) return false;
        i64 outer = 1, inner = 1; for (i64 i = 0; i < ax; ++i) outer *= out.shape[i]; for (i64 i = ax + 1; i < r; ++i) inner *= out.shape[i];
        i64 total = 0;
        for (auto& v : ins) { if (i64(v.shape.size()) != r || !v.data) return false; total += v.shape[ax] * inner;
            if (v.data < out.data + out.size() && out.data < v.data + v.size()) return false; }
        if (total * outer != i64(out.size())) return false;
        if (h.copy2d) {   // one strided copy per input
            bool ok = true; i64 off = 0;
            for (auto& v : ins) { i64 len = v.shape[ax] * inner;
                if (len && !h.copy2d(const_cast<float*>(v.data), v.size(), out.data, out.size(), std::size_t(outer), std::size_t(len), std::size_t(total), std::size_t(off))) { ok = false; break; }
                off += len; }
            if (ok) return true;
        }
        if (outer * i64(ins.size()) > 4096) return false;
        for (i64 o = 0; o < outer; ++o) { i64 off = 0;
            for (auto& v : ins) { i64 len = v.shape[ax] * inner;
                if (len) h.copy(const_cast<float*>(v.data) + o * len, std::size_t(len), out.data, out.size(), std::size_t(o * total + off));
                off += len; } }
        return true;
    }
public:
    tensor output(const std::string& name) const {
        for (auto& [nm, v] : p_.g.outputs()) if (nm == name) {
            auto& vw = p_.views[v]; tensor t(vw.shape); std::copy_n(vw.data, t.size(), t.data.data()); return t;
        }
        raise(errc::not_found, "no output {}", name);
    }
    // want_argmax(true): run() leaves the first output on the device and records its argmax (last_argmax() >= 0)
    // when the backend can; the output's host view is then stale until the next run without it.
    void want_argmax(bool on) { want_argmax_ = on; }
    long long last_argmax() const { return argmax_; }
    tensor_view output_view(const std::string& name) const {
        for (auto& [nm, v] : p_.g.outputs()) if (nm == name) return p_.views[v];
        raise(errc::not_found, "no output {}", name);
    }
private:
    plan& p_;
    state_store* st_;
    bool want_argmax_ = false;
    long long argmax_ = -1;
};

// Shape-bucket plan cache: key = caller-provided bucket string (e.g. "prefill:64/past:0").
class plan_cache {
public:
    template <class Build> plan& get(const std::string& key, Build&& build) {
        auto it = cache_.find(key);
        if (it != cache_.end()) { ++hits_; return *it->second; }
        ++misses_;
        return *(cache_[key] = build());
    }
    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }
    void clear() { cache_.clear(); }
private:
    std::map<std::string, std::unique_ptr<plan>> cache_;
    std::size_t hits_ = 0, misses_ = 0;
};

} // namespace forge::graph
