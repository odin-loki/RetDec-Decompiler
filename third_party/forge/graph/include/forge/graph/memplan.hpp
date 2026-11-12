#pragma once
// Liveness -> static arena offsets, aliasing for views, in-place reuse; exact peak known before running.
#include <forge/graph/ir.hpp>
#include <algorithm>

namespace forge::graph {

struct buffer_slot {
    enum kind_t { arena, external, alias } kind = arena;
    std::size_t offset = 0, bytes = 0;  // arena
    value_id root = no_value;           // alias: storage owner
    int def = 0, last = 0;              // live interval (node indices, inclusive)
};

struct memory_plan {
    std::vector<buffer_slot> slots;     // per value
    std::size_t arena_bytes = 0;        // predicted peak
    std::size_t alignment = 64;
    std::size_t naive_bytes = 0;        // sum of all buffers (no reuse), for reporting
    int in_place = 0, aliases = 0;
};

inline bool is_view_op(const node& n) { return n.kind == node_kind::op && n.op == ops::op::reshape; }
inline bool is_inplace_candidate(const node& n) {
    return n.kind == node_kind::op && ops::info(n.op).cls == ops::op_class::elementwise;
}

inline memory_plan plan_memory(const tensor_graph& g, std::size_t align = 64) {
    memory_plan p; p.alignment = align;
    std::size_t N = g.size();
    p.slots.resize(N);
    auto root = [&](value_id v) { while (p.slots[v].kind == buffer_slot::alias) v = p.slots[v].root; return v; };
    std::vector<int> last(N, -1);
    for (std::size_t i = 0; i < N; ++i) for (auto in : g.at(value_id(i)).inputs) last[in] = int(i);
    for (auto& [nm, v] : g.outputs()) last[v] = int(N);
    for (std::size_t i = 0; i < N; ++i) {
        const auto& n = g.at(value_id(i));
        auto& s = p.slots[i];
        s.def = int(i); s.last = std::max(int(i), last[i]);
        s.bytes = std::size_t(numel(n.shape)) * sizeof(float);
        if (n.kind == node_kind::weight || n.kind == node_kind::constant || n.kind == node_kind::state_append) {
            s.kind = buffer_slot::external; continue;
        }
        if (is_view_op(n)) {
            s.kind = buffer_slot::alias; s.root = n.inputs[0]; ++p.aliases;
            continue;
        }
        if (is_inplace_candidate(n) && !n.inputs.empty()) {
            for (auto in : n.inputs) {
                auto r = root(in);
                const auto& rs = p.slots[r];
                if (rs.kind != buffer_slot::arena || rs.bytes != s.bytes) continue;
                // the input's whole storage group must die at this node
                bool dies = true;
                for (std::size_t j = 0; j < i; ++j)
                    if (root(value_id(j)) == r && p.slots[j].last > int(i)) dies = false;
                if (r != in && p.slots[in].last > int(i)) dies = false;
                if (!dies) continue;
                s.kind = buffer_slot::alias; s.root = r; ++p.in_place;
                break;
            }
            if (s.kind == buffer_slot::alias) continue;
        }
    }
    // propagate liveness of aliases to their roots
    for (std::size_t i = 0; i < N; ++i) if (p.slots[i].kind == buffer_slot::alias) {
        auto r = root(value_id(i));
        p.slots[r].last = std::max(p.slots[r].last, p.slots[i].last);
    }
    // greedy best-fit by size (largest first) over time intervals
    std::vector<value_id> order;
    for (std::size_t i = 0; i < N; ++i) if (p.slots[i].kind == buffer_slot::arena) { order.push_back(value_id(i)); p.naive_bytes += p.slots[i].bytes; }
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return p.slots[a].bytes > p.slots[b].bytes; });
    std::vector<value_id> placed;
    auto up = [&](std::size_t x) { return (x + align - 1) / align * align; };
    for (auto v : order) {
        auto& s = p.slots[v];
        std::vector<std::pair<std::size_t, std::size_t>> busy;
        for (auto o : placed) {
            const auto& t = p.slots[o];
            if (t.def <= s.last && s.def <= t.last) busy.push_back({t.offset, t.offset + up(t.bytes)});
        }
        std::sort(busy.begin(), busy.end());
        std::size_t off = 0;
        for (auto [b, e] : busy) { if (off + up(s.bytes) <= b) break; off = std::max(off, e); }
        s.offset = off;
        p.arena_bytes = std::max(p.arena_bytes, off + up(s.bytes));
        placed.push_back(v);
    }
    return p;
}

// Formal property (checked in tests / by PRISM later): no two simultaneously-live arena buffers overlap.
inline bool plan_is_safe(const memory_plan& p) {
    for (std::size_t i = 0; i < p.slots.size(); ++i) for (std::size_t j = i + 1; j < p.slots.size(); ++j) {
        const auto &a = p.slots[i], &b = p.slots[j];
        if (a.kind != buffer_slot::arena || b.kind != buffer_slot::arena || !a.bytes || !b.bytes) continue;
        bool time = a.def <= b.last && b.def <= a.last;
        bool space = a.offset < b.offset + b.bytes && b.offset < a.offset + a.bytes;
        if (time && space) return false;
        if (a.offset + a.bytes > p.arena_bytes || b.offset + b.bytes > p.arena_bytes) return false;
    }
    return true;
}

} // namespace forge::graph
