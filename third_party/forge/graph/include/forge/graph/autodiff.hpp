#pragma once
// Reverse-mode automatic differentiation on the graph IR. grad() appends the backward graph to `g`, built
// from the same spec ops, so it is optimised, planned and lowered like any forward graph (CPU or GPU).
// Supported ops: add sub mul div neg scale square sqrt rsqrt recip exp log tanh sigmoid silu relu abs,
// matmul (b rank 2), matmul_t, transpose, reshape, reduce_sum, reduce_mean, logsumexp, softmax, rms_norm
// (weight and input). Anything else raises errc::unsupported naming the op.
#include <forge/graph/ir.hpp>
#include <forge/graph/passes.hpp>
#include <map>
#include <numeric>

namespace forge::graph {
namespace detail_ad {
struct builder {
    tensor_graph& g;
    value_id op(ops::op o, std::vector<value_id> in, ops::attrs a = {}) {
        auto v = g.apply(o, std::move(in), std::move(a)); auto& n = g.nodes()[v];
        std::vector<shape_t> ss; for (auto i : n.inputs) ss.push_back(g.at(i).shape);
        n.shape = ops::infer_shape(ops::info(o).shape, ss, n.attrs); return v;
    }
    value_id k(float x) { auto v = g.scalar(x); return v; }
    // Returns a reference into the node vector: valid only until the next g.apply(). Parameters that receive a
    // shape taken from it are therefore taken by value (sum_to, unreduce, reshape), not by reference.
    const shape_t& sh(value_id v) const { return g.at(v).shape; }
    value_id reshape(value_id v, shape_t s) { return sh(v) == s ? v : op(ops::op::reshape, {v}, {{"shape", std::vector<i64>(s.begin(), s.end())}}); }
    // Sum a broadcast-shaped gradient back down to `target` (numpy broadcasting rules).
    value_id sum_to(value_id v, shape_t target) {
        while (sh(v).size() > target.size()) v = op(ops::op::reduce_sum, {v}, {{"axis", 0.0}});
        for (std::size_t ax = 0; ax < target.size(); ++ax)
            if (target[ax] == 1 && sh(v)[ax] != 1) { shape_t keep = sh(v); keep[ax] = 1; v = reshape(op(ops::op::reduce_sum, {v}, {{"axis", double(ax)}}), keep); }
        return reshape(v, target);
    }
    // Broadcast a reduced gradient (axis removed) back to `full`.
    value_id unreduce(value_id v, shape_t full, i64 ax) {
        shape_t keep = full; keep[std::size_t(ax)] = 1; v = reshape(v, keep);
        return full[std::size_t(ax)] == 1 ? v : op(ops::op::expand, {v}, {{"axis", double(ax)}, {"n", double(full[std::size_t(ax)])}});
    }
    // sum over the last axis, keeping it as size 1 and broadcasting back to x's shape
    value_id rowsum_b(value_id v) { i64 ax = i64(sh(v).size()) - 1; return unreduce(op(ops::op::reduce_sum, {v}, {{"axis", double(ax)}}), sh(v), ax); }
};
inline i64 axis_of(const node& n, std::size_t rank) { i64 a = i64(n.attrs.num("axis", -1)); return a < 0 ? a + i64(rank) : a; }
}

// Returns d(loss)/d(v) for each v in `wrt` (value ids in g). `loss` must have one element. Values that do not
// influence the loss get a zero gradient.
inline std::map<value_id, value_id> grad(tensor_graph& g, value_id loss, const std::vector<value_id>& wrt) {
    using ops::op; verify_graph(g);
    if (numel(g.at(loss).shape) != 1) raise(errc::invalid_argument, "grad: loss must have one element, got {}", shape_str(g.at(loss).shape));
    detail_ad::builder b{g};
    std::size_t nfwd = std::size_t(loss) + 1;
    std::map<value_id, value_id> gr;
    auto acc = [&](value_id x, value_id d) { d = b.sum_to(d, g.at(x).shape); auto it = gr.find(x); if (it == gr.end()) gr[x] = d; else it->second = b.op(op::add, {it->second, d}); };
    gr[loss] = g.constant(tensor(g.at(loss).shape, 1.f));
    for (std::size_t i = nfwd; i-- > 0;) {
        auto it = gr.find(value_id(i)); if (it == gr.end()) continue;
        node n = g.at(value_id(i)); value_id y = value_id(i), d = it->second;
        if (n.kind != node_kind::op) continue;
        auto x = n.inputs.empty() ? no_value : n.inputs[0], w = n.inputs.size() > 1 ? n.inputs[1] : no_value;
        switch (n.op) {
        case op::add: acc(x, d); acc(w, d); break;
        case op::sub: acc(x, d); acc(w, b.op(op::neg, {d})); break;
        case op::mul: acc(x, b.op(op::mul, {d, w})); acc(w, b.op(op::mul, {d, x})); break;
        case op::div: acc(x, b.op(op::div, {d, w})); acc(w, b.op(op::neg, {b.op(op::mul, {d, b.op(op::div, {y, w})})})); break;
        case op::neg: acc(x, b.op(op::neg, {d})); break;
        case op::scale: acc(x, b.op(op::scale, {d}, {{"alpha", n.attrs.num("alpha", 1)}})); break;
        case op::square: acc(x, b.op(op::scale, {b.op(op::mul, {d, x})}, {{"alpha", 2.0}})); break;
        case op::sqrt: acc(x, b.op(op::scale, {b.op(op::div, {d, y})}, {{"alpha", 0.5}})); break;
        case op::rsqrt: acc(x, b.op(op::scale, {b.op(op::mul, {d, b.op(op::mul, {y, b.op(op::square, {y})})})}, {{"alpha", -0.5}})); break;
        case op::recip: acc(x, b.op(op::neg, {b.op(op::mul, {d, b.op(op::square, {y})})})); break;
        case op::exp: acc(x, b.op(op::mul, {d, y})); break;
        case op::log: acc(x, b.op(op::div, {d, x})); break;
        case op::tanh: acc(x, b.op(op::sub, {d, b.op(op::mul, {d, b.op(op::square, {y})})})); break;
        case op::sigmoid: acc(x, b.op(op::mul, {d, b.op(op::sub, {y, b.op(op::square, {y})})})); break;
        case op::silu: { auto s = b.op(op::sigmoid, {x});   // s * (1 + x (1 - s))
            auto t = b.op(op::add, {b.k(1.f), b.op(op::mul, {x, b.op(op::sub, {b.k(1.f), s})})});
            acc(x, b.op(op::mul, {d, b.op(op::mul, {s, t})})); break; }
        case op::relu: acc(x, b.op(op::select, {d, b.k(0.f), y})); break;
        case op::abs: acc(x, b.op(op::mul, {d, b.op(op::select, {b.op(op::div, {x, y}), b.k(0.f), y})})); break;
        case op::matmul: {
            auto& bs = g.at(w).shape; if (bs.size() != 2) raise(errc::unsupported, "grad: matmul with batched b");
            acc(x, b.op(op::matmul_t, {d, w}));
            i64 kk = bs[0], nn = bs[1];
            auto a2 = b.reshape(x, {numel(g.at(x).shape) / kk, kk}), d2 = b.reshape(d, {numel(b.sh(d)) / nn, nn});
            acc(w, b.op(op::matmul, {b.op(op::transpose, {a2}, {{"perm", std::vector<i64>{1, 0}}}), d2})); break; }
        case op::matmul_t: {
            auto& ws = g.at(w).shape; i64 kk = ws[1], nn = ws[0];
            acc(x, b.op(op::matmul, {d, w}));
            auto a2 = b.reshape(x, {numel(g.at(x).shape) / kk, kk}), d2 = b.reshape(d, {numel(b.sh(d)) / nn, nn});
            acc(w, b.op(op::matmul, {b.op(op::transpose, {d2}, {{"perm", std::vector<i64>{1, 0}}}), a2})); break; }
        case op::transpose: {
            auto p = n.attrs.ints("perm"); std::vector<i64> inv(p.size()); for (std::size_t j = 0; j < p.size(); ++j) inv[std::size_t(p[j])] = i64(j);
            acc(x, b.op(op::transpose, {d}, {{"perm", inv}})); break; }
        case op::reshape: acc(x, b.reshape(d, g.at(x).shape)); break;
        case op::reduce_sum: acc(x, b.unreduce(d, g.at(x).shape, detail_ad::axis_of(n, g.at(x).shape.size()))); break;
        case op::reduce_mean: { auto& xs = g.at(x).shape; i64 ax = detail_ad::axis_of(n, xs.size());
            acc(x, b.op(op::scale, {b.unreduce(d, xs, ax)}, {{"alpha", 1.0 / double(xs[std::size_t(ax)])}})); break; }
        case op::logsumexp: { auto& xs = g.at(x).shape; i64 ax = detail_ad::axis_of(n, xs.size());
            auto p = b.op(op::exp, {b.op(op::sub, {x, b.unreduce(y, xs, ax)})});
            acc(x, b.op(op::mul, {p, b.unreduce(d, xs, ax)})); break; }
        case op::softmax: { auto gy = b.op(op::mul, {d, y}); acc(x, b.op(op::sub, {gy, b.op(op::mul, {y, b.rowsum_b(gy)})})); break; }
        case op::rms_norm: {   // y = x * r * w, r = (mean(x^2) + eps)^-1/2 over the last axis
            auto& xs = g.at(x).shape; i64 D = xs.back(), ax = i64(xs.size()) - 1; double eps = n.attrs.num("eps", 1e-5);
            auto ms = b.op(op::reduce_mean, {b.op(op::square, {x})}, {{"axis", double(ax)}});
            auto r = b.unreduce(b.op(op::rsqrt, {b.op(op::add, {ms, b.k(float(eps))})}), xs, ax);
            auto xh = b.op(op::mul, {x, r});
            acc(w, b.op(op::mul, {d, xh}));
            auto gw = b.op(op::mul, {d, w});                               // dL/dxhat
            auto dotm = b.op(op::scale, {b.rowsum_b(b.op(op::mul, {gw, xh}))}, {{"alpha", 1.0 / double(D)}});
            acc(x, b.op(op::mul, {r, b.op(op::sub, {gw, b.op(op::mul, {xh, dotm})})})); break; }
        default: raise(errc::unsupported, "grad: no derivative for op '{}'", ops::info(n.op).name);
        }
    }
    std::map<value_id, value_id> out;
    for (auto v : wrt) {
        auto it = gr.find(v);
        out[v] = it != gr.end() ? it->second : g.constant(tensor(g.at(v).shape, 0.f));
    }
    verify_graph(g);
    return out;
}
}
