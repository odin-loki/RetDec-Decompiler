#pragma once
// Op metadata types (class, shape rule, tolerance, fusion) used by the table generated from ops/defs/*.fspec.
// See ops/README.md.
#include <forge/core/tensor.hpp>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace forge::ops {

enum class op_class : u8 { elementwise, reduction, contraction, data_movement, stateful };
enum class shape_rule : u8 { same, broadcast, reduce, matmul, matmul_t, attention, transpose, reshape,
                             gather, concat, slice, expand };
enum class tol_kind : u8 { exact, ulp, rel, abs };
enum class fusion : u8 { none, elementwise, epilogue };

struct tolerance { tol_kind kind; double value; };

struct op_info {
    std::string_view name;
    op_class cls;
    int inputs;
    shape_rule shape;
    tolerance tol;
    fusion fuse;
    double cost;   // flops per output element (contraction: per MAC)
    std::string_view attrs;
    std::string_view doc;
};

using attr_value = std::variant<double, std::vector<i64>, std::string>;

struct attrs {
    std::map<std::string, attr_value, std::less<>> v;
    attrs() = default;
    attrs(std::initializer_list<std::pair<const std::string, attr_value>> il) : v(il) {}
    double num(std::string_view k, double dflt = 0) const {
        auto it = v.find(k);
        if (it == v.end()) return dflt;
        if (auto* d = std::get_if<double>(&it->second)) return *d;
        if (auto* iv = std::get_if<std::vector<i64>>(&it->second); iv && iv->size() == 1) return double((*iv)[0]);
        raise(errc::invalid_argument, "attr '{}' is not a number", k);
    }
    std::vector<i64> ints(std::string_view k) const {
        auto it = v.find(k);
        if (it == v.end()) return {};
        if (auto* iv = std::get_if<std::vector<i64>>(&it->second)) return *iv;
        if (auto* d = std::get_if<double>(&it->second)) return {i64(*d)};
        raise(errc::invalid_argument, "attr '{}' is not an int list", k);
    }
    std::string str(std::string_view k, std::string dflt = {}) const {
        auto it = v.find(k);
        if (it == v.end()) return dflt;
        if (auto* s = std::get_if<std::string>(&it->second)) return *s;
        raise(errc::invalid_argument, "attr '{}' is not a string", k);
    }
    bool has(std::string_view k) const { return v.find(k) != v.end(); }
    void set(std::string k, attr_value a) { v[std::move(k)] = std::move(a); }
    bool operator==(const attrs&) const = default;
};

using ref_fn = tensor (*)(const std::vector<tensor>&, const attrs&);

inline i64 norm_axis(i64 axis, std::size_t rank) {
    i64 r = i64(rank);
    if (axis < 0) axis += r;
    if (axis < 0 || axis >= r) raise(errc::invalid_argument, "axis {} out of range for rank {}", axis, rank);
    return axis;
}

inline shape_t broadcast_shapes(const std::vector<shape_t>& in) {
    std::size_t rank = 0;
    for (auto& s : in) rank = std::max(rank, s.size());
    shape_t out(rank, 1);
    for (auto& s : in) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            auto& o = out[rank - s.size() + i];
            i64 d = s[i];
            if (o == 1) o = d;
            else if (d != 1 && d != o)
                raise(errc::shape_mismatch, "cannot broadcast {} with {}", shape_str(s), shape_str(out));
        }
    }
    return out;
}

// Shape inference driven by the spec's shape rule.
inline shape_t infer_shape(shape_rule r, const std::vector<shape_t>& in, const attrs& a) {
    auto need = [&](std::size_t n) {
        if (in.size() < n) raise(errc::invalid_argument, "expected {} inputs, got {}", n, in.size());
    };
    switch (r) {
    case shape_rule::same: need(1); return in[0];
    case shape_rule::broadcast: return broadcast_shapes(in);
    case shape_rule::reduce: {
        need(1); shape_t s = in[0];
        i64 ax = norm_axis(i64(a.num("axis", -1)), s.size());
        s.erase(s.begin() + ax);
        return s;
    }
    case shape_rule::matmul: case shape_rule::matmul_t: {
        need(2); const auto& x = in[0]; const auto& w = in[1];
        if (x.size() < 2 || w.size() < 2) raise(errc::shape_mismatch, "matmul needs rank>=2");
        bool t = r == shape_rule::matmul_t;
        i64 k = x.back();
        i64 wk = t ? w[w.size() - 1] : w[w.size() - 2];
        i64 n = t ? w[w.size() - 2] : w[w.size() - 1];
        if (k != wk) raise(errc::shape_mismatch, "matmul inner dims differ: {} vs {}", shape_str(x), shape_str(w));
        if (w.size() > 2 && (w.size() != x.size() || !std::equal(w.begin(), w.end() - 2, x.begin())))
            raise(errc::shape_mismatch, "batched matmul batch dims differ");
        shape_t s = x; s.back() = n; return s;
    }
    case shape_rule::attention: {
        need(3); const auto& q = in[0]; const auto& k = in[1]; const auto& v = in[2];
        if (q.size() != 3 || k.size() != 3 || v.size() != 3) raise(errc::shape_mismatch, "attention expects [s,h,d]");
        if (k[0] != v[0] || k[1] != v[1]) raise(errc::shape_mismatch, "attention k/v shapes differ");
        if (q[2] != k[2]) raise(errc::shape_mismatch, "attention head dims differ");
        if (q[1] % k[1]) raise(errc::shape_mismatch, "q heads not multiple of kv heads");
        return shape_t{q[0], q[1], v[2]};  // value head dim may differ (MLA)
    }
    case shape_rule::expand: {  // repeat a size-1 axis n times
        need(1); shape_t s = in[0]; i64 ax = norm_axis(i64(a.num("axis", 0)), s.size());
        if (s[std::size_t(ax)] != 1) raise(errc::shape_mismatch, "expand: axis {} has size {}, need 1", ax, s[std::size_t(ax)]);
        s[std::size_t(ax)] = i64(a.num("n", 1)); return s;
    }
    case shape_rule::transpose: {
        need(1); auto p = a.ints("perm"); shape_t s = in[0];
        if (p.empty()) { std::reverse(s.begin(), s.end()); return s; }
        if (p.size() != s.size()) raise(errc::shape_mismatch, "perm rank mismatch");
        shape_t o(s.size()); for (std::size_t i = 0; i < p.size(); ++i) o[i] = s[std::size_t(norm_axis(p[i], s.size()))];
        return o;
    }
    case shape_rule::reshape: {
        need(1); auto t = a.ints("shape"); i64 known = 1; int neg = -1;
        for (std::size_t i = 0; i < t.size(); ++i) { if (t[i] == -1) neg = int(i); else known *= t[i]; }
        i64 n = numel(in[0]);
        if (neg >= 0) { if (known == 0 || n % known) raise(errc::shape_mismatch, "bad reshape"); t[std::size_t(neg)] = n / known; }
        if (numel(t) != n) raise(errc::shape_mismatch, "reshape {} -> {} changes size", shape_str(in[0]), shape_str(t));
        return t;
    }
    case shape_rule::gather: {
        need(2); if (in[0].size() != 2) raise(errc::shape_mismatch, "gather table must be rank 2");
        shape_t s = in[1]; s.push_back(in[0][1]); return s;
    }
    case shape_rule::concat: {
        need(2); shape_t s = in[0]; i64 ax = norm_axis(i64(a.num("axis", 0)), s.size());
        for (std::size_t j = 1; j < in.size(); ++j) {
            if (in[j].size() != s.size()) raise(errc::shape_mismatch, "concat rank mismatch");
            for (std::size_t i = 0; i < s.size(); ++i)
                if (i64(i) != ax && in[j][i] != s[i]) raise(errc::shape_mismatch, "concat dim mismatch");
            if (j) s[std::size_t(ax)] += in[j][std::size_t(ax)];
        }
        return s;
    }
    case shape_rule::slice: {
        need(1); shape_t s = in[0]; i64 ax = norm_axis(i64(a.num("axis", 0)), s.size());
        i64 st = i64(a.num("start", 0)), en = i64(a.num("end", double(s[std::size_t(ax)])));
        if (st < 0) st += s[std::size_t(ax)];
        if (en < 0) en += s[std::size_t(ax)];
        en = std::min(en, s[std::size_t(ax)]);
        if (st < 0 || st > en) raise(errc::shape_mismatch, "bad slice");
        s[std::size_t(ax)] = en - st; return s;
    }
    }
    raise(errc::unsupported, "unknown shape rule");
}

} // namespace forge::ops
