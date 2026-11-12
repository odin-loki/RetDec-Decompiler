#pragma once
// ONNX -> .fpk importer (FORGE_PLAN Part B "ONNX: partial"). A dependency-free protobuf wire decoder reads
// ModelProto; the graph is translated node-by-node into blueprint text and initializers become weights.
// Supported: static-shape inference graphs (MLPs, classifiers, transformer encoder blocks) built from
//   MatMul Gemm Add Sub Mul Div Pow Max Min Relu Sigmoid Tanh Exp Log Sqrt Neg Abs Erf Gelu Softplus
//   Reciprocal Softmax LayerNormalization Transpose Reshape Flatten Identity Constant ReduceMean ReduceSum
// One symbolic dimension (any dim_param) maps to the blueprint dimension `seq`. Anything else is rejected
// with the op name, so the error says exactly what is missing.
#include <forge/import/hf.hpp>
#include <list>
#include <map>
#include <set>

namespace forge::import {

namespace onnx_pb {

// protobuf wire format: (field << 3 | wire_type) keys; 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32
struct field { std::uint32_t num = 0, wt = 0; std::uint64_t v = 0; std::string_view bytes; };

class cursor {
public:
    explicit cursor(std::string_view s) : s_(s) {}
    bool done() const { return p_ >= s_.size(); }
    std::uint64_t varint() {
        std::uint64_t v = 0;
        for (int sh = 0; sh < 64; sh += 7) {
            if (p_ >= s_.size()) raise(errc::format_error, "onnx: truncated varint");
            auto b = std::uint8_t(s_[p_++]);
            v |= std::uint64_t(b & 0x7f) << sh;
            if (!(b & 0x80)) return v;
        }
        raise(errc::format_error, "onnx: varint too long");
    }
    field next() {
        field f; auto key = varint(); f.num = std::uint32_t(key >> 3); f.wt = std::uint32_t(key & 7);
        switch (f.wt) {
        case 0: f.v = varint(); break;
        case 1: need(8); std::memcpy(&f.v, s_.data() + p_, 8); p_ += 8; break;
        case 5: { need(4); std::uint32_t x; std::memcpy(&x, s_.data() + p_, 4); f.v = x; p_ += 4; break; }
        case 2: { auto n = varint(); need(n); f.bytes = s_.substr(p_, std::size_t(n)); p_ += std::size_t(n); break; }
        default: raise(errc::format_error, "onnx: unsupported wire type {}", f.wt);
        }
        return f;
    }
private:
    void need(std::uint64_t n) { if (n > s_.size() - p_) raise(errc::format_error, "onnx: truncated field"); }
    std::string_view s_; std::size_t p_ = 0;
};

template <class F> void each(std::string_view s, F&& f) { cursor c(s); while (!c.done()) f(c.next()); }

// repeated scalar fields may be packed (wt 2) or not
inline void ints_of(const field& f, std::vector<i64>& out) {
    if (f.wt == 2) { cursor c(f.bytes); while (!c.done()) out.push_back(i64(c.varint())); }
    else out.push_back(i64(f.v));
}
inline void floats_of(const field& f, std::vector<float>& out) {
    auto put = [&](std::uint32_t u) { float x; std::memcpy(&x, &u, 4); out.push_back(x); };
    if (f.wt == 2) {
        if (f.bytes.size() % 4) raise(errc::format_error, "onnx: packed floats not a multiple of 4");
        for (std::size_t i = 0; i < f.bytes.size(); i += 4) { std::uint32_t u; std::memcpy(&u, f.bytes.data() + i, 4); put(u); }
    } else put(std::uint32_t(f.v));
}

struct tensor_t { std::string name; shape_t dims; int dtype = 0; std::vector<float> f; std::vector<i64> i; };
struct attr_t { std::string name; float f = 0; i64 i = 0; std::string s; std::vector<float> fl; std::vector<i64> il; std::optional<tensor_t> t; int type = 0; };
struct node_t { std::string op, name, domain; std::vector<std::string> in, out; std::map<std::string, attr_t> attrs; };
struct vinfo_t { std::string name; int elem = 0; std::vector<i64> dims; std::vector<bool> symbolic; };
struct model_t { i64 opset = 0; std::string producer; std::vector<node_t> nodes; std::vector<tensor_t> inits; std::vector<vinfo_t> inputs, outputs; };

inline tensor_t parse_tensor(std::string_view s) {
    tensor_t t; std::string_view raw; std::vector<i64> i32;
    each(s, [&](const field& f) {
        switch (f.num) {
        case 1: ints_of(f, t.dims); break;
        case 2: t.dtype = int(f.v); break;
        case 4: floats_of(f, t.f); break;
        case 5: ints_of(f, i32); break;
        case 7: ints_of(f, t.i); break;
        case 8: t.name = std::string(f.bytes); break;
        case 9: raw = f.bytes; break;
        case 13: raise(errc::unsupported, "onnx: tensor {} uses external data (not supported; re-save with all tensors inline)", t.name);
        }
    });
    i64 n = 1; for (auto d : t.dims) { if (d < 0 || d > (i64(1) << 40)) raise(errc::format_error, "onnx: tensor {} has bad dims", t.name); n *= d; if (n > (i64(1) << 40)) raise(errc::format_error, "onnx: tensor {} too large", t.name); }
    auto take = [&](std::size_t w, auto conv) {
        if (raw.size() != std::size_t(n) * w) raise(errc::format_error, "onnx: tensor {} raw_data has {} bytes, expected {}", t.name, raw.size(), std::size_t(n) * w);
        for (std::size_t k = 0; k < std::size_t(n); ++k) conv(raw.data() + k * w);
    };
    switch (t.dtype) {
    case 1: if (!raw.empty()) take(4, [&](const char* p) { float x; std::memcpy(&x, p, 4); t.f.push_back(x); }); break;            // FLOAT
    case 11: if (!raw.empty()) take(8, [&](const char* p) { double x; std::memcpy(&x, p, 8); t.f.push_back(float(x)); }); break;  // DOUBLE
    case 7: if (!raw.empty()) take(8, [&](const char* p) { std::int64_t x; std::memcpy(&x, p, 8); t.i.push_back(x); }); break;    // INT64
    case 6: if (!raw.empty()) take(4, [&](const char* p) { std::int32_t x; std::memcpy(&x, p, 4); t.i.push_back(x); }); else t.i = i32; break;
    case 10: if (!raw.empty()) take(2, [&](const char* p) { std::uint16_t h; std::memcpy(&h, p, 2); t.f.push_back(float(f16::from_bits(h))); }); break;
    default: raise(errc::unsupported, "onnx: tensor {} has data type {} (supported: float, double, float16, int32, int64)", t.name, t.dtype);
    }
    if (t.dtype == 6 || t.dtype == 7) { if (i64(t.i.size()) != n) raise(errc::format_error, "onnx: tensor {} has {} values, expected {}", t.name, t.i.size(), n); }
    else if (i64(t.f.size()) != n) raise(errc::format_error, "onnx: tensor {} has {} values, expected {}", t.name, t.f.size(), n);
    return t;
}

inline attr_t parse_attr(std::string_view s) {
    attr_t a;
    each(s, [&](const field& f) {
        switch (f.num) {
        case 1: a.name = std::string(f.bytes); break;
        case 2: { std::uint32_t u = std::uint32_t(f.v); std::memcpy(&a.f, &u, 4); break; }
        case 3: a.i = i64(f.v); break;
        case 4: a.s = std::string(f.bytes); break;
        case 5: a.t = parse_tensor(f.bytes); break;
        case 7: floats_of(f, a.fl); break;
        case 8: ints_of(f, a.il); break;
        case 20: a.type = int(f.v); break;
        }
    });
    return a;
}

inline vinfo_t parse_vinfo(std::string_view s) {
    vinfo_t v;
    each(s, [&](const field& f) {
        if (f.num == 1) v.name = std::string(f.bytes);
        else if (f.num == 2) each(f.bytes, [&](const field& tp) {           // TypeProto
            if (tp.num != 1) return;                                          // tensor_type
            each(tp.bytes, [&](const field& tt) {
                if (tt.num == 1) v.elem = int(tt.v);
                else if (tt.num == 2) each(tt.bytes, [&](const field& sh) {  // TensorShapeProto
                    if (sh.num != 1) return;
                    i64 val = -1; bool sym = true;
                    each(sh.bytes, [&](const field& d) { if (d.num == 1) { val = i64(d.v); sym = false; } });
                    v.dims.push_back(val); v.symbolic.push_back(sym);
                });
            });
        });
    });
    return v;
}

inline model_t parse_model(std::string_view s) {
    model_t m; bool graph = false;
    each(s, [&](const field& f) {
        if (f.num == 2) m.producer = std::string(f.bytes);
        else if (f.num == 8) {  // opset_import
            std::string dom; i64 ver = 0;
            each(f.bytes, [&](const field& o) { if (o.num == 1) dom = std::string(o.bytes); else if (o.num == 2) ver = i64(o.v); });
            if (dom.empty() || dom == "ai.onnx") m.opset = ver;
        } else if (f.num == 7) {
            graph = true;
            each(f.bytes, [&](const field& g) {
                switch (g.num) {
                case 1: {
                    node_t n;
                    each(g.bytes, [&](const field& nf) {
                        switch (nf.num) {
                        case 1: n.in.emplace_back(nf.bytes); break;
                        case 2: n.out.emplace_back(nf.bytes); break;
                        case 3: n.name = std::string(nf.bytes); break;
                        case 4: n.op = std::string(nf.bytes); break;
                        case 5: { auto a = parse_attr(nf.bytes); n.attrs[a.name] = std::move(a); break; }
                        case 7: n.domain = std::string(nf.bytes); break;
                        }
                    });
                    m.nodes.push_back(std::move(n)); break;
                }
                case 5: m.inits.push_back(parse_tensor(g.bytes)); break;
                case 11: m.inputs.push_back(parse_vinfo(g.bytes)); break;
                case 12: m.outputs.push_back(parse_vinfo(g.bytes)); break;
                }
            });
        }
    });
    if (!graph) raise(errc::format_error, "onnx: no graph in model");
    return m;
}

} // namespace onnx_pb

// Translates an ONNX model to (blueprint text, weights). Shapes are tracked with -1 for the `seq` dimension.
struct onnx_translation {
    std::string blueprint;
    std::vector<std::pair<std::string, onnx_pb::tensor_t>> weights;
    json::array inputs, outputs;  // [{onnx, name, shape}] with -1 = seq
};

inline onnx_translation translate_onnx_impl(const onnx_pb::model_t& m) {
    using namespace onnx_pb;
    onnx_translation tr;
    std::map<std::string, const tensor_t*> consts;
    std::list<tensor_t> owned;
    for (auto& t : m.inits) consts[t.name] = &t;
    std::map<std::string, std::string> ident;   // onnx value name -> blueprint expression
    std::map<std::string, std::vector<i64>> shp; // tracked shapes, -1 = seq
    std::set<std::string> used_ids;
    auto sanitize = [&](const std::string& base, const char* prefix) {
        std::string s = prefix;
        for (char c : base) s += std::isalnum((unsigned char)c) ? c : '_';
        static const std::set<std::string> reserved{"W", "seq", "repeat", "in", "graph", "input", "output", "state", "params", "session", "blueprint", "opset"};
        while (used_ids.count(s) || reserved.count(s)) s += "_";
        used_ids.insert(s); return s;
    };
    std::set<std::string> weight_used;
    auto weight = [&](const std::string& onnx_name) {
        auto* t = consts.at(onnx_name);
        auto id = "w_" + std::to_string(tr.weights.size());
        if (!weight_used.insert(onnx_name).second) { for (auto& [k, w] : tr.weights) if (w.name == onnx_name) return "W." + k; }
        tr.weights.emplace_back(id, *t);
        if (tr.weights.back().second.dims.empty()) tr.weights.back().second.dims = {1};  // scalars broadcast as [1]
        if (tr.weights.back().second.f.empty()) for (auto v : t->i) tr.weights.back().second.f.push_back(float(v));
        return "W." + id;
    };
    std::string body;
    std::string seq_param;
    auto dimtxt = [](const std::vector<i64>& d) {
        std::string s; for (std::size_t i = 0; i < d.size(); ++i) s += (i ? ", " : "") + (d[i] < 0 ? std::string("seq") : std::to_string(d[i]));
        return s;
    };
    for (auto& in : m.inputs) {
        if (consts.count(in.name)) continue;  // old exporters list initializers as inputs
        if (in.elem != 1 && in.elem != 0) raise(errc::unsupported, "onnx: input {} has element type {} (only float inputs are supported)", in.name, in.elem);
        int nsym = 0; for (bool b : in.symbolic) nsym += b;
        if (nsym > 1) raise(errc::unsupported, "onnx: input {} has {} symbolic dims (at most one, mapped to `seq`)", in.name, nsym);
        auto id = sanitize(in.name, "in_");
        ident[in.name] = id; shp[in.name] = in.dims;
        tr.inputs.push_back(json::object{{"onnx", in.name}, {"name", id}, {"shape", [&] { json::array a; for (auto d : in.dims) a.push_back(double(d)); return a; }()}});
    }
    auto ex = [&](const std::string& n) -> std::string {
        if (auto it = ident.find(n); it != ident.end()) return it->second;
        if (consts.count(n)) { shp[n] = consts.at(n)->dims; return ident[n] = weight(n); }
        raise(errc::not_found, "onnx: value '{}' is used before it is defined", n);
    };
    auto rank_of = [&](const std::string& n) { ex(n); return i64(shp.at(n).size()); };
    auto bcast = [](std::vector<i64> a, std::vector<i64> b) {
        if (a.size() < b.size()) std::swap(a, b);
        for (std::size_t i = 0; i < b.size(); ++i) { auto& x = a[a.size() - b.size() + i]; auto y = b[i]; if (x == 1) x = y; }
        return a;
    };
    auto ints_const = [&](const std::string& n, const char* what) -> std::vector<i64> {
        auto it = consts.find(n);
        if (it == consts.end()) raise(errc::unsupported, "onnx: {} input '{}' must be a constant", what, n);
        if (!it->second->i.empty()) return it->second->i;
        std::vector<i64> r; for (auto v : it->second->f) r.push_back(i64(v)); return r;
    };
    auto scalar_const = [&](const std::string& n) -> std::optional<float> {
        auto it = consts.find(n);
        if (it == consts.end()) return std::nullopt;
        auto& t = *it->second; i64 c = 1; for (auto d : t.dims) c *= d;
        if (c != 1) return std::nullopt;
        return t.f.empty() ? float(t.i[0]) : t.f[0];
    };
    auto fmtf = [](double v) { return std::format("{:.9g}", v); };
    auto attr_i = [](const node_t& n, const char* k, i64 d) { auto it = n.attrs.find(k); return it == n.attrs.end() ? d : it->second.i; };
    auto attr_f = [](const node_t& n, const char* k, double d) { auto it = n.attrs.find(k); return it == n.attrs.end() ? d : double(it->second.f); };
    auto norm_axis = [](i64 ax, i64 r) { return ax < 0 ? ax + r : ax; };
    static const std::map<std::string, std::string> unary{
        {"Relu", "relu"}, {"Sigmoid", "sigmoid"}, {"Tanh", "tanh"}, {"Exp", "exp"}, {"Log", "log"}, {"Sqrt", "sqrt"}, {"Neg", "neg"},
        {"Abs", "abs"}, {"Erf", "erf"}, {"Softplus", "softplus"}, {"Reciprocal", "recip"}, {"Sin", "sin"}, {"Cos", "cos"}};
    static const std::map<std::string, std::string> binary{{"Add", "add"}, {"Sub", "sub"}, {"Mul", "mul"}, {"Div", "div"}, {"Pow", "pow"}, {"Max", "maximum"}, {"Min", "minimum"}};

    for (auto& n : m.nodes) {
        if (!n.domain.empty() && n.domain != "ai.onnx") raise(errc::unsupported, "onnx: node {} uses custom domain '{}'", n.name, n.domain);
        if (n.out.empty()) continue;
        const auto& o = n.out[0];
        std::string e; std::vector<i64> os;
        auto X = [&](std::size_t i) { if (i >= n.in.size() || n.in[i].empty()) raise(errc::format_error, "onnx: {} node {} is missing input {}", n.op, n.name, i); return ex(n.in[i]); };
        auto S = [&](std::size_t i) { X(i); return shp.at(n.in[i]); };
        if (n.op == "Constant") {
            auto it = n.attrs.find("value");
            if (it == n.attrs.end() || !it->second.t) raise(errc::unsupported, "onnx: Constant node {} without a tensor value", n.name);
            owned.push_back(*it->second.t); owned.back().name = o; consts[o] = &owned.back();
            continue;
        } else if (unary.count(n.op)) { e = unary.at(n.op) + "(" + X(0) + ")"; os = S(0); }
        else if (n.op == "Identity") { ident[o] = X(0); shp[o] = S(0); continue; }
        else if (n.op == "Gelu") {
            auto ap = n.attrs.count("approximate") ? n.attrs.at("approximate").s : "none";
            e = (ap == "tanh" ? "gelu_tanh(" : "gelu(") + X(0) + ")"; os = S(0);
        } else if (binary.count(n.op)) {
            auto sb = scalar_const(n.in.at(1));
            if (sb && n.op == "Mul") { e = "scale(" + X(0) + ", alpha: " + fmtf(*sb) + ")"; os = S(0); }
            else if (sb && n.op == "Div") { e = "scale(" + X(0) + ", alpha: " + fmtf(1.0 / *sb) + ")"; os = S(0); }
            else { e = binary.at(n.op) + "(" + X(0) + ", " + X(1) + ")"; os = bcast(S(0), S(1)); }
        } else if (n.op == "MatMul") {
            auto a = S(0), b = S(1);
            if (b.size() < 2 || a.size() < 2) raise(errc::unsupported, "onnx: MatMul {} needs rank >= 2 operands", n.name);
            e = "matmul(" + X(0) + ", " + X(1) + ")"; os = a; os.back() = b.back();
        } else if (n.op == "Gemm") {
            if (attr_i(n, "transA", 0)) raise(errc::unsupported, "onnx: Gemm {} with transA=1", n.name);
            bool tb = attr_i(n, "transB", 0); auto a = S(0), b = S(1);
            if (a.size() != 2 || b.size() != 2) raise(errc::unsupported, "onnx: Gemm {} needs rank-2 operands", n.name);
            e = std::string(tb ? "matmul_t(" : "matmul(") + X(0) + ", " + X(1) + ")";
            os = {a[0], tb ? b[0] : b[1]};
            if (double al = attr_f(n, "alpha", 1); al != 1) e = "scale(" + e + ", alpha: " + fmtf(al) + ")";
            if (n.in.size() > 2 && !n.in[2].empty()) {
                double be = attr_f(n, "beta", 1);
                std::string c = X(2); if (be != 1) c = "scale(" + c + ", alpha: " + fmtf(be) + ")";
                e = "add(" + e + ", " + c + ")";
            }
        } else if (n.op == "Softmax") {
            auto s = S(0); i64 r = i64(s.size());
            i64 ax = norm_axis(attr_i(n, "axis", m.opset >= 13 ? -1 : 1), r);
            if (ax != r - 1) raise(errc::unsupported, "onnx: Softmax {} over axis {} (only the last axis is supported)", n.name, ax);
            e = "softmax(" + X(0) + ")"; os = s;
        } else if (n.op == "LayerNormalization") {
            auto s = S(0);
            if (norm_axis(attr_i(n, "axis", -1), i64(s.size())) != i64(s.size()) - 1) raise(errc::unsupported, "onnx: LayerNormalization {} must normalise the last axis", n.name);
            std::string b = n.in.size() > 2 && !n.in[2].empty() ? X(2) : std::string();
            if (b.empty()) {  // synthesise a zero bias
                owned.push_back(onnx_pb::tensor_t{n.name + ".zero_bias", {s.back()}, 1, std::vector<float>(std::size_t(s.back()), 0.f), {}});
                consts[owned.back().name] = &owned.back(); b = ex(owned.back().name);
            }
            e = "layer_norm(" + X(0) + ", " + X(1) + ", " + b + ", eps: " + fmtf(attr_f(n, "epsilon", 1e-5)) + ")"; os = s;
        } else if (n.op == "Transpose") {
            auto s = S(0); std::vector<i64> perm;
            if (n.attrs.count("perm")) perm = n.attrs.at("perm").il; else for (i64 i = i64(s.size()) - 1; i >= 0; --i) perm.push_back(i);
            std::string p; for (std::size_t i = 0; i < perm.size(); ++i) { p += (i ? ", " : "") + std::to_string(perm[i]); os.push_back(s.at(std::size_t(perm[i]))); }
            e = "transpose(" + X(0) + ", perm: [" + p + "])";
        } else if (n.op == "Reshape" || n.op == "Flatten") {
            auto s = S(0); std::vector<i64> want;
            if (n.op == "Flatten") {
                i64 ax = norm_axis(attr_i(n, "axis", 1), i64(s.size())), a = 1, b = 1; bool sa = false, sb = false;
                for (i64 i = 0; i < i64(s.size()); ++i) { auto d = s[std::size_t(i)]; if (i < ax) { if (d < 0) sa = true; else a *= d; } else { if (d < 0) sb = true; else b *= d; } }
                want = {sa ? -1 : a, sb ? -1 : b};
                if ((sa && a != 1) || (sb && b != 1)) raise(errc::unsupported, "onnx: Flatten {} would fold `seq` with other dims", n.name);
            } else {
                want = ints_const(n.in.at(1), "Reshape shape");
                i64 known = 1; bool seq_in = false; for (auto d : s) { if (d < 0) seq_in = true; else known *= d; }
                int neg = 0;
                for (std::size_t i = 0; i < want.size(); ++i) {
                    if (want[i] == 0) want[i] = s.at(i);
                    if (want[i] == -1) ++neg;
                }
                if (neg && seq_in) raise(errc::unsupported, "onnx: Reshape {} uses -1 on a `seq`-shaped tensor", n.name);
                if (neg == 1 && !seq_in) { i64 k = 1; for (auto d : want) if (d != -1) k *= d; for (auto& d : want) if (d == -1) d = known / k; }
            }
            std::string l; for (std::size_t i = 0; i < want.size(); ++i) l += (i ? ", " : "") + (want[i] < 0 ? std::string("seq") : std::to_string(want[i]));
            e = "reshape(" + X(0) + ", shape: [" + l + "])"; os = want;
        } else if (n.op == "ReduceMean" || n.op == "ReduceSum") {
            auto s = S(0); std::vector<i64> axes;
            if (n.attrs.count("axes")) axes = n.attrs.at("axes").il; else if (n.in.size() > 1 && !n.in[1].empty()) axes = ints_const(n.in[1], "Reduce axes");
            if (axes.size() != 1 || norm_axis(axes[0], i64(s.size())) != i64(s.size()) - 1 || !attr_i(n, "keepdims", 1))
                raise(errc::unsupported, "onnx: {} {} must reduce only the last axis with keepdims=1", n.op, n.name);
            os = s; os.back() = 1;  // FORGE reductions drop the axis; reshape restores keepdims
            e = std::string(n.op == "ReduceMean" ? "reshape(reduce_mean(" : "reshape(reduce_sum(") + X(0) + "), shape: [" + dimtxt(os) + "])";
        } else raise(errc::unsupported, "onnx: operator '{}' (node {}) is not supported by the FORGE importer", n.op, n.name.empty() ? "?" : n.name);
        auto id = sanitize(o, "t_");
        body += "  " + id + " = " + e + "\n";
        ident[o] = id; shp[o] = os;
    }
    std::string ins, outs, outs_body;
    for (auto& i : tr.inputs) ins += "input " + i["name"].str() + " : f32[" + [&] { std::vector<i64> d; for (auto& x : i["shape"].arr()) d.push_back(i64(x.num())); return dimtxt(d); }() + "]\n";
    for (auto& o : m.outputs) {
        auto src = ex(o.name);
        auto id = sanitize(o.name, "out_");
        outs_body += "  " + id + " = " + src + "\n";
        auto s = shp.at(o.name);
        outs += "output " + id + " : f32[" + dimtxt(s) + "]\n";
        json::array a; for (auto d : s) a.push_back(double(d));
        tr.outputs.push_back(json::object{{"onnx", o.name}, {"name", id}, {"shape", a}});
    }
    tr.blueprint = "# Translated from ONNX by forge import\nblueprint \"onnx\" opset 1\n\n" + ins + "\ngraph {\n" + body + outs_body + "}\n\n" + outs + "session one_shot\n";
    return tr;
}

// malformed graphs (bad perms, out-of-range axes) surface as a format error rather than a library exception
inline onnx_translation translate_onnx(const onnx_pb::model_t& m) {
    try { return translate_onnx_impl(m); }
    catch (const std::out_of_range& e) { raise(errc::format_error, "onnx: malformed graph ({})", e.what()); }
}

inline import_report import_onnx(const std::string& path, const std::string& out, const import_options& opt = {}) {
    check_write(out);
    auto bytes = read_file(path);   // checks fs_read (hf.hpp)
    auto m = onnx_pb::parse_model(bytes);
    auto tr = translate_onnx(m);
    auto bp = modelfile::parse_blueprint(tr.blueprint);  // validate syntax before writing
    modelfile::writer w;
    import_report rep{"onnx", "onnx", 0, 0};
    for (auto& [k, t] : tr.weights) {
        auto codec = (opt.codec == "f32" || t.dims.size() < 2) ? std::string("f32") : opt.codec;
        w.add_tensor(k, t.dims, t.f, codec); ++rep.tensors;
    }
    json::object man{{"format", "fpk"}, {"forge_format_version", double(modelfile::format_version)}, {"architecture", "onnx"},
                     {"source", "onnx"}, {"producer", m.producer}, {"onnx_opset", double(m.opset)}, {"session", bp.session},
                     {"onnx_inputs", tr.inputs}, {"onnx_outputs", tr.outputs}};
    w.set_manifest(json::value(man));
    w.set_section("blueprint", tr.blueprint);
    w.write(out);
    rep.bytes = std::filesystem::file_size(out);
    return rep;
}

} // namespace forge::import
