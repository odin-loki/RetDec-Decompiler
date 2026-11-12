#include <cmath>
#include <cstring>
#include <map>
#include "check.hpp"
#include <forge/graph/graph.hpp>
#include <random>

using namespace forge;
using namespace forge::graph;

TEST(verify_reports_location) {
    tensor_graph g;
    auto a = g.input("a", {3, 4}), b = g.input("b", {5, 4});
    g.output("y", g.matmul(a, b));
    bool threw = false;
    try { verify_graph(g); } catch (const forge_exception& e) { threw = true; CHECK(std::string(e.what()).find("node %2 (matmul)") != std::string::npos); }
    CHECK(threw);
}
TEST(cse_fold_simplify_fuse_dce) {
    tensor_graph g;
    auto x = g.input("x", {4, 8});
    auto c = g.apply(ops::op::add, {g.scalar(1.f), g.scalar(2.f)});      // folds to 3
    auto e1 = g.apply(ops::op::exp, {x}), e2 = g.apply(ops::op::exp, {x}); // CSE
    auto s = g.apply(ops::op::silu, {e1});
    auto y = g.mul(s, e2);                                                   // fuse -> silu_mul
    auto y1 = g.mul(y, g.scalar(1.f));                                       // simplify away
    g.apply(ops::op::tanh, {x});                                            // dead
    g.output("y", g.add(y1, c));
    auto o = optimise(g);
    int n_exp = 0, n_silu_mul = 0, n_tanh = 0, n_mul = 0;
    for (auto& n : o.nodes()) if (n.kind == node_kind::op) {
        n_exp += n.op == ops::op::exp; n_silu_mul += n.op == ops::op::silu_mul; n_tanh += n.op == ops::op::tanh; n_mul += n.op == ops::op::mul;
    }
    CHECK(n_exp == 1); CHECK(n_silu_mul == 1); CHECK(n_tanh == 0); CHECK(n_mul == 0);
    // numerics: optimised == unoptimised
    std::mt19937 r(1); auto X = verify::random_tensor({4, 8}, r, -2, 2);
    auto p1 = compile(g), p2 = compile(g, nullptr, {false, false, false, false});
    executor e1x(*p1), e2x(*p2);
    e1x.set_input("x", X.data); e2x.set_input("x", X.data); e1x.run(); e2x.run();
    auto c1 = verify::compare(e1x.output("y"), e2x.output("y"), {ops::tol_kind::rel, 1e-5});
    CHECK_MSG(c1.ok, "%s", c1.message.c_str());
}

// Pass equivalence on random graphs.
TEST(pass_equivalence_random_graphs) {
    std::mt19937 r(77);
    const ops::op un[] = {ops::op::exp, ops::op::tanh, ops::op::sigmoid, ops::op::silu, ops::op::neg, ops::op::square};
    const ops::op bin[] = {ops::op::add, ops::op::mul, ops::op::sub, ops::op::maximum};
    for (int trial = 0; trial < 40; ++trial) {
        tensor_graph g; std::vector<value_id> vals = {g.input("x", {3, 17}), g.input("y", {3, 17})};
        for (int i = 0; i < 12; ++i) {
            if (r() % 2) vals.push_back(g.apply(un[r() % 6], {vals[r() % vals.size()]}));
            else vals.push_back(g.apply(bin[r() % 4], {vals[r() % vals.size()], vals[r() % vals.size()]}));
            vals.back() = g.apply(ops::op::tanh, {vals.back()}); // keep values bounded
        }
        g.output("o", vals.back());
        g.output("p", vals[vals.size() / 2]);
        auto X = verify::random_tensor({3, 17}, r, -1, 1), Y = verify::random_tensor({3, 17}, r, -1, 1);
        auto a = compile(g), b = compile(g, nullptr, {false, false, false, false});
        CHECK(plan_is_safe(a->mem));
        executor ea(*a), eb(*b);
        for (auto* e : {&ea, &eb}) { e->set_input("x", X.data); e->set_input("y", Y.data); e->run(); }
        for (auto nm : {"o", "p"}) {
            auto c = verify::compare(ea.output(nm), eb.output(nm), {ops::tol_kind::abs, 1e-5});
            CHECK_MSG(c.ok, "trial %d %s: %s", trial, nm, c.message.c_str());
        }
    }
}

TEST(memory_plan_reuse_and_peak) {
    tensor_graph g;
    auto x = g.input("x", {1024});
    value_id v = x;
    for (int i = 0; i < 20; ++i) v = g.apply(i % 2 ? ops::op::exp : ops::op::tanh, {v});
    g.output("y", v);
    auto p = compile(g);
    CHECK(plan_is_safe(p->mem));
    std::printf("  arena %zu naive %zu in-place %d\n", p->mem.arena_bytes, p->mem.naive_bytes, p->mem.in_place);
    CHECK(p->mem.arena_bytes <= 2 * 4096);             // chain reuses buffers
    CHECK(p->mem.naive_bytes >= p->mem.arena_bytes && p->mem.in_place == 20);
    // predicted peak == measured: the executor's arena is exactly the predicted size
    CHECK(p->arena.size() * sizeof(float) >= p->mem.arena_bytes && p->arena.size() * sizeof(float) <= p->mem.arena_bytes + 4);
}

TEST(lowerings_pass_oracle) {
    auto& reg = registry::cpu();
    int n = 0;
    for (std::size_t o = 0; o < ops::op_count; ++o)
        for (auto& l : reg.lowerings(ops::op(o))) {
            std::string msg;
            bool ok = registry::oracle_check(l, &msg);
            CHECK_MSG(ok, "%s: %s", l.name.c_str(), msg.c_str());
            ++n;
        }
    std::printf("  %d lowerings verified\n", n);
}

// Oracle matrix: every spec op through the full compile pipeline vs its reference.
TEST(oracle_matrix_all_ops) {
    std::mt19937 rng(99);
    int ok = 0;
    for (std::size_t o = 0; o < ops::op_count; ++o) {
        auto c = verify::make_case(ops::op(o), rng);
        tensor_graph g; std::vector<value_id> ins;
        for (std::size_t i = 0; i < c.in.size(); ++i) ins.push_back(g.input("i" + std::to_string(i), c.in[i].shape));
        g.output("y", g.apply(ops::op(o), ins, c.a));
        auto p = compile(g, nullptr, {false, false, false, false});
        executor e(*p);
        for (std::size_t i = 0; i < c.in.size(); ++i) e.set_input("i" + std::to_string(i), c.in[i].data);
        e.run();
        auto ref = ops::reference(ops::op(o))(c.in, c.a);
        auto r = verify::compare(e.output("y"), ref, ops::info(ops::op(o)).tol);
        CHECK_MSG(r.ok, "%s: %s", std::string(ops::info(ops::op(o)).name).c_str(), r.message.c_str());
        ok += r.ok;
    }
    std::printf("  %d/%zu ops pass\n", ok, ops::op_count);
}

TEST(state_append_symbolic_length) {
    state_store st; st.capacity = 16;
    tensor_graph g;
    auto x = g.input("x", {1, 2, 4});
    auto kv = g.state_append("k0", x);
    g.output("y", g.apply(ops::op::attention, {x, kv, kv}, {{"causal", 1.0}}));
    auto p = compile(g, &st);
    executor e(*p, &st);
    std::vector<float> in(8, 0.5f);
    for (int t = 0; t < 5; ++t) { e.set_input("x", in); e.run(); }
    CHECK(st.slots["k0"].len == 5);
    auto y = e.output("y");
    for (float v : y.data) CHECK(std::fabs(v - 0.5f) < 1e-6f);
}

// Fixed-size state ops: prefill in one run == token-by-token runs (state carried in the slot), against a
// float64 reference of the gated delta rule (l2norm on q/k, GQA-style key head sharing).
TEST(state_ops_conv_and_delta_rule_streaming) {
    const i64 S = 7, Hk = 2, Hv = 4, dk = 8, dv = 6, C = 10, K = 4;
    std::mt19937 rng(11); std::normal_distribution<float> nd(0.f, 1.f);
    auto rnd = [&](std::size_t n) { std::vector<float> v(n); for (auto& x : v) x = nd(rng); return v; };
    auto q = rnd(S * Hk * dk), k = rnd(S * Hk * dk), v = rnd(S * Hv * dv), gl = rnd(S * Hv), bl = rnd(S * Hv);
    for (auto& x : gl) x = -std::fabs(x) * 0.5f;                       // log decay <= 0
    for (auto& x : bl) x = 1.f / (1.f + std::exp(-x));                 // beta in (0, 1)
    auto xc = rnd(S * C), wc = rnd(C * K);
    // reference
    std::vector<double> Sref(std::size_t(Hv * dk * dv), 0.0), oref(std::size_t(S * Hv * dv)), cref(std::size_t(S * C));
    for (i64 t = 0; t < S; ++t) {
        for (i64 c = 0; c < C; ++c) { double a = 0; for (i64 j = 0; j < K; ++j) { i64 tt = t - (K - 1) + j; if (tt >= 0) a += double(wc[std::size_t(c * K + j)]) * xc[std::size_t(tt * C + c)]; } cref[std::size_t(t * C + c)] = a; }
        for (i64 h = 0; h < Hv; ++h) {
            i64 kh = h / (Hv / Hk); std::vector<double> qn(static_cast<std::size_t>(dk)), kn(static_cast<std::size_t>(dk)); double qs = 0, ks = 0;
            for (i64 i = 0; i < dk; ++i) { double a = q[std::size_t((t * Hk + kh) * dk + i)], b = k[std::size_t((t * Hk + kh) * dk + i)]; qn[std::size_t(i)] = a; kn[std::size_t(i)] = b; qs += a * a; ks += b * b; }
            for (i64 i = 0; i < dk; ++i) { qn[std::size_t(i)] /= std::sqrt(qs + 1e-6) * std::sqrt(double(dk)); kn[std::size_t(i)] /= std::sqrt(ks + 1e-6); }
            double* Sh = Sref.data() + h * dk * dv; double dec = std::exp(double(gl[std::size_t(t * Hv + h)])), be = bl[std::size_t(t * Hv + h)];
            for (i64 x = 0; x < dk * dv; ++x) Sh[x] *= dec;
            for (i64 j = 0; j < dv; ++j) {
                double kv = 0; for (i64 i = 0; i < dk; ++i) kv += Sh[i * dv + j] * kn[std::size_t(i)];
                double d = (v[std::size_t((t * Hv + h) * dv + j)] - kv) * be;
                for (i64 i = 0; i < dk; ++i) Sh[i * dv + j] += kn[std::size_t(i)] * d;
                double o = 0; for (i64 i = 0; i < dk; ++i) o += Sh[i * dv + j] * qn[std::size_t(i)];
                oref[std::size_t((t * Hv + h) * dv + j)] = o;
            }
        }
    }
    auto build = [&](i64 s) {
        tensor_graph g;
        auto iq = g.input("q", {s, Hk, dk}), ik = g.input("k", {s, Hk, dk}), iv = g.input("v", {s, Hv, dv});
        auto ig = g.input("g", {s, Hv}), ib = g.input("b", {s, Hv}), ix = g.input("x", {s, C});
        auto w = g.constant(tensor({C, K}, 0.f)); g.at(w).constant->data = wc;
        g.output("o", g.state_op("ssm", "delta_rule", {iq, ik, iv, ig, ib}));
        g.output("c", g.state_op("conv", "causal_conv", {ix, w}));
        return g;
    };
    auto feed = [&](executor& e, i64 t0, i64 s) {
        auto sl = [&](const std::vector<float>& a, i64 row) { return std::span<const float>(a.data() + t0 * row, std::size_t(s * row)); };
        e.set_input("q", sl(q, Hk * dk)); e.set_input("k", sl(k, Hk * dk)); e.set_input("v", sl(v, Hv * dv));
        e.set_input("g", sl(gl, Hv)); e.set_input("b", sl(bl, Hv)); e.set_input("x", sl(xc, C)); e.run();
    };
    double eo = 0, ec = 0;
    {   // whole prompt in one run
        state_store st; auto p = compile(build(S), &st); executor e(*p, &st); feed(e, 0, S);
        auto o = e.output("o"), c = e.output("c");
        for (std::size_t i = 0; i < oref.size(); ++i) eo = std::max(eo, std::fabs(o.data[i] - oref[i]));
        for (std::size_t i = 0; i < cref.size(); ++i) ec = std::max(ec, std::fabs(c.data[i] - cref[i]));
        CHECK(st.slots["ssm"].len == S && st.slots["ssm"].fixed);
        CHECK_THROWS(st.truncate(3));                                      // recurrent state cannot rewind
    }
    {   // 3-token prefill then one token at a time; then reset() restarts from zeros
        state_store st; auto p3 = compile(build(3), &st), p1 = compile(build(1), &st);
        executor e3(*p3, &st), e1(*p1, &st);
        for (int pass = 0; pass < 2; ++pass) {
            feed(e3, 0, 3);
            for (i64 t = 0; t < 3; ++t) for (i64 j = 0; j < Hv * dv; ++j) eo = std::max(eo, std::fabs(e3.output("o").data[std::size_t(t * Hv * dv + j)] - oref[std::size_t(t * Hv * dv + j)]));
            for (i64 t = 3; t < S; ++t) {
                feed(e1, t, 1);
                auto o = e1.output("o"), c = e1.output("c");
                for (i64 j = 0; j < Hv * dv; ++j) eo = std::max(eo, std::fabs(o.data[std::size_t(j)] - oref[std::size_t(t * Hv * dv + j)]));
                for (i64 j = 0; j < C; ++j) ec = std::max(ec, std::fabs(c.data[std::size_t(j)] - cref[std::size_t(t * C + j)]));
            }
            st.reset();
        }
    }
    std::printf("  state ops: delta_rule max err %.2e, causal_conv max err %.2e\n", eo, ec);
    CHECK(eo < 1e-4); CHECK(ec < 1e-4);
}

// Chunked CPU attention (attn_cpu.hpp): GQA, f32 and int8 KV, contexts spanning several 256-position chunks.
// Against a float64 reference over the same (decoded) KV; a prefill block gives the same bits as each token
// alone; every seq kernel table (scalar / avx2 / avx512) gives the same bits.
TEST(attn_cpu_chunked_long_context) {
    std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 1.f);
    for (int codec = 0; codec < 2; ++codec)
    for (auto [t, s, hq, hk, d, window] : std::vector<std::array<std::size_t, 6>>{{700, 40, 8, 2, 64, 0}, {1300, 3, 16, 4, 80, 0}, {600, 9, 4, 4, 32, 300}}) {
        auto kc = codec ? kv_codec::int8 : kv_codec::f32;
        std::size_t dv = d, w = codec ? std::size_t(kv_head_words(i64(d))) : d;
        std::vector<float> q(s * hq * d), kf(t * hk * d), vf(t * hk * dv), kq(t * hk * w), vq(t * hk * w), kd(t * hk * d), vd(t * hk * dv);
        for (auto& x : q) x = nd(rng);
        for (auto& x : kf) x = nd(rng);
        for (auto& x : vf) x = nd(rng);
        for (std::size_t r = 0; r < t * hk; ++r) {
            if (codec) { kv_encode_head(kf.data() + r * d, i64(d), kc, kq.data() + r * w); kv_encode_head(vf.data() + r * dv, i64(dv), kc, vq.data() + r * w);
                         kv_decode_head(kq.data() + r * w, i64(d), kc, kd.data() + r * d); kv_decode_head(vq.data() + r * w, i64(dv), kc, vd.data() + r * dv); }
        }
        if (!codec) { kq = kf; vq = vf; kd = kf; vd = vf; }
        float sc = 1.f / std::sqrt(float(d));
        std::vector<float> out(s * hq * dv), one(hq * dv);
        attn_cpu::attention(q.data(), s, hq, d, kq.data(), vq.data(), t, hk, dv, kc, kc, true, window, sc, out.data());
        double err = 0;
        for (std::size_t i = 0; i < s; ++i) for (std::size_t h = 0; h < hq; ++h) {
            std::size_t pos = t - s + i, lo = window && pos + 1 > window ? pos + 1 - window : 0, kh = h / (hq / hk);
            std::vector<double> sco(pos + 1); double mx = -1e300, sum = 0;
            for (std::size_t j = lo; j <= pos; ++j) { double a = 0; for (std::size_t e = 0; e < d; ++e) a += double(q[(i * hq + h) * d + e]) * kd[(j * hk + kh) * d + e]; sco[j] = a * sc; mx = std::max(mx, sco[j]); }
            for (std::size_t j = lo; j <= pos; ++j) { sco[j] = std::exp(sco[j] - mx); sum += sco[j]; }
            for (std::size_t e = 0; e < dv; ++e) { double a = 0; for (std::size_t j = lo; j <= pos; ++j) a += sco[j] * vd[(j * hk + kh) * dv + e]; err = std::max(err, std::fabs(a / sum - out[(i * hq + h) * dv + e])); }
            // the same token alone (decode) gives identical bits
            if (i == s - 1 || i == 0) {
                attn_cpu::attention(q.data() + i * hq * d, 1, hq, d, kq.data(), vq.data(), pos + 1, hk, dv, kc, kc, true, window, sc, one.data());
                CHECK(std::memcmp(one.data(), out.data() + i * hq * dv, hq * dv * 4) == 0);
            }
        }
        CHECK_MSG(err < 2e-5, "attn_cpu err %g", err);
        // every kernel table gives the same bits
        const auto* ref = simd::seq_by_name("scalar");
        for (const char* tg : {"avx2", "avx512"}) {
            const auto* k = simd::seq_by_name(tg); if (!k) continue;
            std::size_t ns = hq / hk;
            std::vector<const float*> qp; std::vector<std::size_t> lo(ns, 0), hi(ns, t);
            for (std::size_t h = 0; h < ns; ++h) qp.push_back(q.data() + h * d);
            std::vector<float> m1(ns), l1(ns), a1(ns * dv), m2(ns), l2(ns), a2(ns * dv), sc1(ref->attn_scratch(ns, d, dv)), sc2(k->attn_scratch(ns, d, dv));
            simd::seq_kv K{kq.data(), hk * w, codec}, V{vq.data(), hk * w, codec};
            ref->attn_chunk(K, V, d, dv, sc, 256, std::min<std::size_t>(t, 512), ns, qp.data(), lo.data(), hi.data(), m1.data(), l1.data(), a1.data(), sc1.data());
            k->attn_chunk(K, V, d, dv, sc, 256, std::min<std::size_t>(t, 512), ns, qp.data(), lo.data(), hi.data(), m2.data(), l2.data(), a2.data(), sc2.data());
            CHECK(m1 == m2 && l1 == l2 && std::memcmp(a1.data(), a2.data(), a1.size() * 4) == 0);
            for (float x : {-0.f, -1.5f, -30.f, -79.f, 3.f}) CHECK(ref->exp1(x) == k->exp1(x));
        }
    }
}

TEST(codecs_exact_and_layout) {
    // values exactly representable in a codec survive the round trip bit-for-bit
    auto rt = [](std::string_view name, std::vector<float> x) {
        auto* c = codecs::find(name);
        std::vector<u8> b(c->bytes(x.size())); std::vector<float> y(x.size());
        c->encode(x.data(), x.size(), b.data()); c->decode(b.data(), x.size(), y.data());
        for (std::size_t i = 0; i < x.size(); ++i) CHECK_MSG(y[i] == x[i], "%s[%zu] %g -> %g", std::string(name).c_str(), i, x[i], y[i]);
    };
    std::vector<float> t(64); for (int i = 0; i < 64; ++i) t[i] = float((i % 3) - 1) * 0.5f;
    rt("tq2", t);                                                    // ternary data at one magnitude is exact
    std::vector<float> m4(32); const float e2m1[8] = {0, 0.5f, 1, 1.5f, 2, 3, 4, 6};
    for (int i = 0; i < 32; ++i) m4[i] = e2m1[i % 8] * (i & 8 ? -1.f : 1.f) * 0.25f;
    rt("mxfp4", m4);                                                  // amax 1.5 -> X = -2, so every value is an exact E2M1 * 2^-2
    std::vector<float> g(32); for (int i = 0; i < 32; ++i) g[i] = float(i % 16) * 0.125f - 1.f;
    rt("q4_g32", g);                                                  // 16 levels, step 0.125, min -1
    CHECK(codecs::find("mxfp8")->bytes(64) == 66 && codecs::find("nvfp4")->bytes(16) == 9 && codecs::find("q4_g32")->bytes(33) == 40);
    for (auto& c : codecs::all()) CHECK(dtype_from_name(c.name) == c.tag);
}

TEST(codecs_roundtrip) {
    std::mt19937 r(4);
    auto x = verify::random_tensor({1000}, r, -3, 3);
    for (auto& c : codecs::all()) {
        std::vector<u8> buf(c.bytes(x.size()));
        std::vector<float> y(x.size());
        c.encode(x.data.data(), x.size(), buf.data());
        c.decode(buf.data(), x.size(), y.data());
        double maxe = 0; for (std::size_t i = 0; i < y.size(); ++i) maxe = std::max(maxe, double(std::fabs(y[i] - x.data[i])));
        static const std::map<std::string_view, double> bounds = {
            {"f32", 0}, {"f16", 2e-3}, {"bf16", 2e-2}, {"fp8_e4m3", 0.13}, {"fp8_e5m2", 0.26}, {"q8_0", 0.013}, {"q4_k", 0.25},
            {"q4_g32", 0.21}, {"q8_g32", 0.013}, {"mxfp8", 0.126}, {"mxfp6", 0.126}, {"mxfp4", 0.51}, {"mxint8", 0.016},
            {"nvfp4", 0.51}, {"tq2", 3.0}, {"q6_k", 0.07}, {"q5_k", 0.13}};
        CHECK_MSG(bounds.count(c.name), "no bound for %s", std::string(c.name).c_str());
        double bound = bounds.count(c.name) ? bounds.at(c.name) : 0;
        std::printf("  %-9s %5zu bytes  max err %.5f\n", std::string(c.name).c_str(), buf.size(), maxe);
        CHECK_MSG(maxe <= bound, "%s", std::string(c.name).c_str());
    }
}


// Native quantised kernels (lane/qdot.hpp) must be bit-identical to the scalar codec decode + lane dot, on every
// simd target this machine can run, including a partial final tile (k not a multiple of 256 for q8_0).
TEST(qdot_matches_decode_exactly) {
    std::mt19937 r(11);
    { const auto& f = simd::detect();   // logged so a SIGILL in a VM shows which features the guest reports
      std::printf("  cpu: avx=%d avx2=%d fma=%d f16c=%d bmi2=%d popcnt=%d avx512f=%d bw=%d dq=%d vl=%d cd=%d\n", f.avx, f.avx2, f.fma, f.f16c,
                  f.bmi2, f.popcnt, f.avx512f, f.avx512bw, f.avx512dq, f.avx512vl, f.avx512cd); std::fflush(stdout); }
    for (const char* tgt : {"scalar", "avx2", "avx512"}) {
        const auto* ker = simd::by_name(tgt);
        if (!ker) { std::printf("  qdot: target %s not available\n", tgt); continue; }
        for (const char* cn : {"q8_0", "q4_k", "q6_k", "q5_k", "f16", "q8_g32"}) {
            const auto* c = codecs::find(cn);
            for (std::size_t k : {std::size_t(256), std::size_t(1024), std::size_t(768 + (c->block == 32 ? 96 : 0))}) {
                auto w = verify::random_tensor({i64(k)}, r, -2, 2); auto x = verify::random_tensor({i64(k)}, r, -1, 1);
                std::vector<u8> enc(c->bytes(k)); c->encode(w.data.data(), k, enc.data());
                std::vector<float> ref(k), got(k);
                c->decode(enc.data(), k, ref.data());
                int fmt = qweights::fmt_of(c);
                ker->qdecode(fmt, enc.data(), k, got.data(), c->decode);
                CHECK_MSG(std::memcmp(ref.data(), got.data(), k * 4) == 0, "%s/%s k=%zu: qdecode differs", tgt, cn, k);
                float a = ker->dot(x.data.data(), ref.data(), k);
                float b = ker->qdot(fmt, enc.data(), x.data.data(), k, c->decode, c->block, c->block_bytes);
                CHECK_MSG(std::memcmp(&a, &b, 4) == 0, "%s/%s k=%zu: qdot %.9g vs dot %.9g", tgt, cn, k, double(b), double(a));
            }
        }
        std::printf("  qdot: %s exact for q8_0 q4_k q6_k q5_k f16 q8_g32\n", tgt);
    }
}

// Integer matvec (simd/qdot_i8.hpp): every ISA table this machine runs gives the scalar reference's bits, for
// quantisation and products, and a vector's result does not depend on how many vectors ran with it. The
// result stays close to the f32 product of the decoded weights (int8 activations: ~0.5% of sum |x*w|).
TEST(qdot_i8_matches_reference_on_every_isa) {
    std::mt19937 r(5);
    const auto* ref = simd::qi8_by_name("scalar");
    CHECK(ref != nullptr);
    std::printf("  qi8 active: %s\n", simd::qi8_active().target);
    for (const char* cn : {"q8_0", "q4_k", "q5_k", "q6_k"}) {
        const auto* c = codecs::find(cn); int fmt = qweights::fmt_of(c);
        for (std::size_t k : {std::size_t(256), std::size_t(1280)}) {
            const std::size_t rows = 37, nvmax = 13, rb = c->bytes(k), ab = simd::qi8::act_bytes(fmt, k);
            CHECK(simd::qi8::supported(fmt, k));
            auto W = verify::random_tensor({i64(rows), i64(k)}, r, -2, 2);
            auto X = verify::random_tensor({i64(nvmax), i64(k)}, r, -1, 1);
            for (std::size_t i = 0; i < X.data.size(); i += 7) X.data[i] *= 1e-3f;   // small values next to large ones
            std::vector<u8> enc(rows * rb);
            for (std::size_t j = 0; j < rows; ++j) c->encode(W.data.data() + j * k, k, enc.data() + j * rb);
            std::vector<simd::qi8::act_k> abuf_ref((nvmax * ab) / sizeof(simd::qi8::act_k) + 1), abuf((nvmax * ab) / sizeof(simd::qi8::act_k) + 1);
            auto* aref = reinterpret_cast<u8*>(abuf_ref.data()); auto* aq = reinterpret_cast<u8*>(abuf.data());
            for (std::size_t v = 0; v < nvmax; ++v) ref->quantize(fmt, X.data.data() + v * k, k, aref + v * ab);
            std::vector<float> yref(nvmax * rows), y(nvmax * rows), y1(rows);
            ref->matvec(fmt, enc.data(), rb, rows, aref, ab, k, nvmax, yref.data(), rows);
            // accuracy vs f32 decode + dot
            std::vector<float> wd(k);
            for (std::size_t j = 0; j < rows; ++j) {
                c->decode(enc.data() + j * rb, k, wd.data());
                for (std::size_t v = 0; v < nvmax; ++v) {
                    double exact = 0, mag = 0;
                    for (std::size_t i = 0; i < k; ++i) { exact += double(wd[i]) * X.data[v * k + i]; mag += std::fabs(double(wd[i]) * X.data[v * k + i]); }
                    CHECK_MSG(std::fabs(yref[v * rows + j] - exact) <= 0.01 * mag + 1e-6, "%s k=%zu row %zu vec %zu: %g vs %g (|.|=%g)", cn, k, j, v,
                              double(yref[v * rows + j]), exact, mag);
                }
            }
            for (const char* tgt : {"scalar", "avx2", "avxvnni", "avx512vnni"}) {
                const auto* ker = simd::qi8_by_name(tgt);
                if (!ker) continue;
                for (std::size_t v = 0; v < nvmax; ++v) ker->quantize(fmt, X.data.data() + v * k, k, aq + v * ab);
                CHECK_MSG(std::memcmp(aq, aref, nvmax * ab) == 0, "%s/%s k=%zu: quantised activations differ", tgt, cn, k);
                for (std::size_t nv : {std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(5), std::size_t(8), std::size_t(9), nvmax}) {
                    std::fill(y.begin(), y.end(), -1.f);
                    ker->matvec(fmt, enc.data(), rb, rows, aref, ab, k, nv, y.data(), rows);
                    CHECK_MSG(std::memcmp(y.data(), yref.data(), nv * rows * 4) == 0, "%s/%s k=%zu nv=%zu: differs from the scalar reference", tgt, cn, k, nv);
                }
                for (std::size_t v = 0; v < nvmax; ++v) {   // one vector at a time, in odd row slices
                    for (std::size_t r0 = 0; r0 < rows; r0 += 11) ker->matvec(fmt, enc.data() + r0 * rb, rb, std::min<std::size_t>(11, rows - r0), aref + v * ab, ab, k, 1, y1.data() + r0, rows);
                    CHECK_MSG(std::memcmp(y1.data(), yref.data() + v * rows, rows * 4) == 0, "%s/%s k=%zu: vector %zu alone differs from the batch", tgt, cn, k, v);
                }
            }
        }
    }
    // the whole graph path: a batch of activation rows gives each row's single-row bits
    for (const char* cn : {"q4_k", "q6_k", "q8_0"}) {
        const auto* c = codecs::find(cn); int fmt = qweights::fmt_of(c);
        const std::size_t n = 300, k = 512, m = 21, rb = c->bytes(k);
        auto W = verify::random_tensor({i64(n), i64(k)}, r, -1, 1); auto X = verify::random_tensor({i64(m), i64(k)}, r, -1, 1);
        std::vector<u8> enc(n * rb); for (std::size_t j = 0; j < n; ++j) c->encode(W.data.data() + j * k, k, enc.data() + j * rb);
        std::vector<float> yb(m * n), ys(n);
        qweights::gemm_nt_i8(fmt, enc.data(), rb, n, X.data.data(), m, k, yb.data());
        for (std::size_t i = 0; i < m; ++i) {
            qweights::gemm_nt_i8(fmt, enc.data(), rb, n, X.data.data() + i * k, 1, k, ys.data());
            CHECK_MSG(std::memcmp(ys.data(), yb.data() + i * n, n * 4) == 0, "%s: gemm_nt_i8 row %zu differs between m=%zu and m=1", cn, i, m);
        }
    }
}

#include <forge/graph/autodiff.hpp>
// Reverse-mode gradients vs central finite differences (double-precision forward via the same plan in f32,
// so the tolerance is loose but the check catches wrong formulas immediately).
TEST(autodiff_matches_finite_differences) {
    using namespace forge::graph; using ops::op;
    auto build = [](tensor_graph& g, std::vector<value_id>& ins) {
        auto x = g.input("x", {3, 4}), W = g.input("W", {5, 4}), V = g.input("V", {5, 2}), bvec = g.input("b", {5}), nw = g.input("nw", {4}), c = g.input("c", {3, 1});
        ins = {x, W, V, bvec, nw, c};
        auto h = g.apply(op::rms_norm, {x, nw}, {{"eps", 1e-5}});
        auto z = g.add(g.apply(op::matmul_t, {h, W}), bvec);                       // [3,5]
        auto t = g.apply(op::tanh, {z}), s = g.apply(op::silu, {z}), sg = g.apply(op::sigmoid, {t});
        auto sm = g.apply(op::softmax, {g.add(t, s)});
        auto m = g.matmul(g.mul(sm, sg), V);                                         // [3,2]
        auto q = g.apply(op::div, {g.apply(op::square, {m}), g.add(g.apply(op::exp, {c}), g.scalar(1.f))});
        auto r = g.apply(op::relu, {g.apply(op::sub, {q, g.scalar(0.01f)})});
        auto lse = g.apply(op::logsumexp, {g.apply(op::transpose, {g.add(r, g.apply(op::log, {g.add(g.apply(op::abs, {m}), g.scalar(1.f))}))}, {{"perm", std::vector<i64>{1, 0}}})}, {{"axis", 1.0}});
        auto loss = g.apply(op::reduce_mean, {g.apply(op::sqrt, {g.add(g.apply(op::reshape, {lse}, {{"shape", std::vector<i64>{2}}}), g.scalar(4.f))})}, {{"axis", 0.0}});
        return loss;
    };
    // std::normal_distribution differs between libstdc++/libc++/MSVC; Box-Muller on raw mt19937 output is portable,
    // so every platform checks the same point (away from the relu/abs kinks where finite differences are meaningless).
    std::mt19937 rng(11);
    auto N = [&] { double u1 = (double(rng()) + 1.0) / 4294967297.0, u2 = double(rng()) / 4294967296.0; return float(std::sqrt(-2 * std::log(u1)) * std::cos(6.283185307179586 * u2)); };
    std::vector<std::vector<float>> data; for (i64 n : {12, 20, 10, 5, 4, 3}) { std::vector<float> v(static_cast<std::size_t>(n)); for (auto& e : v) e = N(); data.push_back(v); }
    const char* names[] = {"x", "W", "V", "b", "nw", "c"};
    tensor_graph g; std::vector<value_id> ins; auto loss = build(g, ins);
    auto gr = grad(g, loss, ins);
    g.output("loss", loss); for (std::size_t i = 0; i < ins.size(); ++i) g.output(std::string("d") + names[i], gr[ins[i]]);
    auto p = compile(g); executor ex(*p);
    auto run = [&](const std::vector<std::vector<float>>& d) { for (std::size_t i = 0; i < d.size(); ++i) ex.set_input(names[i], d[i]); ex.run(); return double(ex.output_view("loss").data[0]); };
    run(data);
    std::vector<std::vector<float>> an; for (auto nm : names) { auto v = ex.output_view(std::string("d") + nm); an.emplace_back(v.data, v.data + v.size()); }
    double worst = 0;
    for (std::size_t i = 0; i < data.size(); ++i) for (std::size_t j = 0; j < data[i].size(); ++j) {
        auto d2 = data; float h = 2e-3f; d2[i][j] += h; double fp = run(d2); d2[i][j] -= 2 * h; double fm = run(d2);
        double fd = (fp - fm) / (2 * h), e = std::fabs(fd - an[i][j]) / std::max(1e-2, std::fabs(fd));
        worst = std::max(worst, e); CHECK_MSG(e < 3e-2, "d%s[%zu]: analytic %g fd %g", names[i], j, double(an[i][j]), fd);
    }
    std::printf("  autodiff worst relative error vs finite differences: %.2e\n", worst);
    tensor_graph g2; auto a = g2.input("a", {4}); auto am = g2.apply(op::argmax, {a}, {{"axis", 0.0}});
    CHECK_THROWS(grad(g2, am, {a}));
    tensor_graph g3; auto u = g3.input("u", {2}), v = g3.input("v", {2}); auto l3 = g3.apply(op::reduce_sum, {g3.apply(op::exp, {u})}, {{"axis", 0.0}});
    auto z3 = grad(g3, l3, {u, v}); CHECK(g3.at(z3[v]).kind == node_kind::constant);
}

// Resident device backends have no quantised kernels: compile() decodes their quantised weights to f32 only up to
// FORGE_GPU_WEIGHT_BUDGET and keeps the rest encoded for the CPU kernels (a no-op residency stands in for a GPU).
TEST(resident_backend_quantised_weight_budget) {
    auto setb = [](const char* v) {
#ifdef _WIN32
        _putenv_s("FORGE_GPU_WEIGHT_BUDGET", v);
#else
        setenv("FORGE_GPU_WEIGHT_BUDGET", v, 1);
#endif
    };
    const auto* c = codecs::find("q8_0");
    CHECK(c); if (!c) return;
    const i64 n = 8, k = 64;
    std::vector<float> w(std::size_t(n * k));
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = std::sin(float(i) * 0.37f);
    std::vector<std::uint8_t> enc(std::size_t(n) * c->bytes(std::size_t(k)));
    c->encode(w.data(), w.size(), enc.data());
    tensor_graph g;
    auto x = g.input("x", {2, k});
    g.output("y", g.apply(ops::op::matmul_t, {x, g.qweight("w", {n, k}, enc.data(), "q8_0")}));
    auto reg = registry::make();
    auto h = std::make_shared<device_hooks>();
    h->begin_run = [] {}; h->end_run = [] {};
    h->pin = h->to_host = h->host_wrote = h->release = [](const float*, std::size_t) {};
    reg->hooks = h;
    pass_options po; po.lowerings = reg.get();
    std::vector<float> xs(std::size_t(2 * k));
    for (std::size_t i = 0; i < xs.size(); ++i) xs[i] = std::cos(float(i) * 0.11f);
    auto run = [&](const char* budget, bool& encoded) {
        setb(budget);
        auto p = compile(g, nullptr, po);
        encoded = false;
        for (auto& v : p->views) if (v.qdata) encoded = true;
        executor e(*p); e.set_input("x", xs); e.run();
        return e.output("y");
    };
    bool q0 = false, q1 = false;
    auto y0 = run("0", q0), y1 = run("1G", q1);
    setb("");
    CHECK(q0);    // budget 0: the weight stays encoded (CPU quantised matmul)
    CHECK(!q1);   // ample budget: decoded to f32 for the device
    // the encoded CPU path quantises activations to int8 (simd/qdot_i8.hpp) unless FORGE_CPU_INT8=0
    auto cmp = verify::compare(y0, y1, {ops::tol_kind::rel, qweights::int8_enabled() ? 1e-2 : 1e-4});
    CHECK_MSG(cmp.ok, "%s", cmp.message.c_str());
}
