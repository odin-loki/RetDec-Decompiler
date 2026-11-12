#pragma once
// graph IR: a tensor graph treated as a program to compile once and replay.
#include <forge/ops/ops.gen.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace forge::graph {

using value_id = std::uint32_t;
inline constexpr value_id no_value = ~0u;

enum class node_kind : u8 { input, weight, constant, op, state_append, output, state_op };

struct node {
    node_kind kind = node_kind::op;
    ops::op op{};                    // for kind == op
    std::vector<value_id> inputs;
    ops::attrs attrs;
    shape_t shape;                    // filled by verify
    std::string name;                 // inputs/weights/state slots
    const float* weight = nullptr;    // kind == weight: external (e.g. mmap'd / decoded) data
    const std::uint8_t* qweight = nullptr;  // kind == weight: encoded data instead (see plan.hpp: matmul_t reads it in place)
    std::string qcodec;                     // codec name for qweight
    std::shared_ptr<tensor> constant; // kind == constant
    int device = 0;                   // filled by partition
};

class tensor_graph {
public:
    value_id input(std::string name, shape_t s) { node n; n.kind = node_kind::input; n.shape = std::move(s); n.name = std::move(name); return push(std::move(n)); }
    value_id weight(std::string name, shape_t s, const float* data) {
        node n; n.kind = node_kind::weight; n.shape = std::move(s); n.name = std::move(name); n.weight = data; return push(std::move(n));
    }
    value_id qweight(std::string name, shape_t s, const std::uint8_t* data, std::string codec) {
        node n; n.kind = node_kind::weight; n.shape = std::move(s); n.name = std::move(name); n.qweight = data; n.qcodec = std::move(codec);
        return push(std::move(n));
    }
    value_id constant(tensor t) {
        node n; n.kind = node_kind::constant; n.shape = t.shape;
        n.constant = std::make_shared<tensor>(std::move(t));
        return push(std::move(n));
    }
    value_id scalar(float v) { return constant(tensor({1}, v)); }
    value_id apply(ops::op o, std::vector<value_id> in, ops::attrs a = {}) {
        node n; n.kind = node_kind::op; n.op = o; n.inputs = std::move(in); n.attrs = std::move(a); return push(std::move(n));
    }
    value_id apply(std::string_view opname, std::vector<value_id> in, ops::attrs a = {}) {
        auto o = ops::op_from_name(opname);
        if (!o) raise(errc::not_found, "unknown op '{}'", opname);
        return apply(*o, std::move(in), std::move(a));
    }
    // Stateful append along axis 0 to slot `slot`; returns the full history (state ++ x).
    value_id state_append(std::string slot, value_id x) {
        node n; n.kind = node_kind::state_append; n.inputs = {x}; n.name = std::move(slot); return push(std::move(n));
    }
    // Stateful op over a fixed-size mutable slot `slot` (recurrent state, conv window): reads and updates the
    // slot in place each run and returns a fresh value. `fn` names the kernel (see state_ops.hpp).
    value_id state_op(std::string slot, std::string fn, std::vector<value_id> in, ops::attrs a = {}) {
        node n; n.kind = node_kind::state_op; n.inputs = std::move(in); n.name = std::move(slot);
        n.attrs = std::move(a); n.attrs.set("fn", std::move(fn)); return push(std::move(n));
    }
    void output(std::string name, value_id v) { outputs_.push_back({std::move(name), v}); }

    // fluent helpers
    value_id add(value_id a, value_id b) { return apply(ops::op::add, {a, b}); }
    value_id mul(value_id a, value_id b) { return apply(ops::op::mul, {a, b}); }
    value_id matmul(value_id a, value_id b) { return apply(ops::op::matmul, {a, b}); }

    std::vector<node>& nodes() { return nodes_; }
    const std::vector<node>& nodes() const { return nodes_; }
    node& at(value_id v) { return nodes_.at(v); }
    const node& at(value_id v) const { return nodes_.at(v); }
    std::vector<std::pair<std::string, value_id>>& outputs() { return outputs_; }
    const std::vector<std::pair<std::string, value_id>>& outputs() const { return outputs_; }
    std::size_t size() const { return nodes_.size(); }
private:
    value_id push(node n) { nodes_.push_back(std::move(n)); return value_id(nodes_.size() - 1); }
    std::vector<node> nodes_;
    std::vector<std::pair<std::string, value_id>> outputs_;
};

std::string to_string(const tensor_graph& g);

inline std::string to_string(const tensor_graph& g) {
    std::string s;
    for (std::size_t i = 0; i < g.size(); ++i) {
        const auto& n = g.at(value_id(i));
        s += "%" + std::to_string(i) + " " + shape_str(n.shape) + " = ";
        switch (n.kind) {
            case node_kind::input: s += "input " + n.name; break;
            case node_kind::weight: s += "weight " + n.name; break;
            case node_kind::constant: s += "const"; break;
            case node_kind::state_append: s += "state_append " + n.name; break;
            case node_kind::output: s += "output"; break;
            case node_kind::state_op: s += "state_op " + n.attrs.str("fn") + " " + n.name; break;
            case node_kind::op: s += std::string(ops::info(n.op).name); break;
        }
        for (auto in : n.inputs) s += " %" + std::to_string(in);
        s += "\n";
    }
    for (auto& [nm, v] : g.outputs()) s += "output " + nm + " = %" + std::to_string(v) + "\n";
    return s;
}

} // namespace forge::graph
