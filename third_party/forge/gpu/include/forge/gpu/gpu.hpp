#pragma once
// gpu: graph-first GPU programming model. CPU backend implemented; CUDA/HIP/Level Zero/Vulkan/Metal
// backends are discovered by dynamic loading and report "absent" when their runtimes are missing.
#include <forge/core/dtypes.hpp>
#include <forge/core/error.hpp>
#include <forge/simd/dispatch.hpp>
#include <forge/task/pool.hpp>
#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <forge/core/dl.hpp>

#define FORGE_KERNEL

namespace forge::gpu {

template <class T> using span = std::span<T>;

struct extent {
    std::array<i64, 3> n{1, 1, 1};
    extent(i64 x, i64 y = 1, i64 z = 1) : n{x, y, z} {}
    i64 total() const { return n[0] * n[1] * n[2]; }
};

template <int N> struct item {
    std::array<i64, 3> gid{}, ext{1, 1, 1}, lid{}, grp{};
    i64 global(int d) const { return gid[std::size_t(d)]; }
    i64 local(int d) const { return lid[std::size_t(d)]; }
    i64 group(int d) const { return grp[std::size_t(d)]; }
    i64 range(int d) const { return ext[std::size_t(d)]; }
};

enum class backend_kind { cpu, cuda, hip, level_zero, vulkan, metal };
inline const char* name(backend_kind k) {
    switch (k) { case backend_kind::cpu: return "cpu"; case backend_kind::cuda: return "cuda"; case backend_kind::hip: return "hip";
                 case backend_kind::level_zero: return "level_zero"; case backend_kind::vulkan: return "vulkan"; default: return "metal"; }
}

// Normalised capability report.
struct capabilities {
    backend_kind kind = backend_kind::cpu;
    std::string name;
    int compute_units = 1;
    int subgroup_width = 1;
    std::size_t group_memory = 0;
    std::size_t memory_bytes = 0;
    bool unified_memory = true;
    bool matrix_unit = false;
    bool fp16 = false, bf16 = false, fp8 = false;
    double score = 0;   // loader score: higher is better
};

struct backend_probe { backend_kind kind; bool available; std::string detail; };

// Dynamic loader: probe vendor runtimes without linking them. One binary runs anywhere.
inline std::vector<backend_probe> probe_backends() {
    std::vector<backend_probe> r;
    r.push_back({backend_kind::cpu, true, simd::active().target});
    struct lib { backend_kind k; const char* so; };
    for (auto [k, so] : {lib{backend_kind::cuda, FORGE_DLNAME("libcuda.so.1", "nvcuda.dll")}, lib{backend_kind::hip, FORGE_DLNAME("libamdhip64.so", "amdhip64.dll")},
                         lib{backend_kind::level_zero, FORGE_DLNAME("libze_loader.so.1", "ze_loader.dll")}, lib{backend_kind::vulkan, FORGE_DLNAME("libvulkan.so.1", "vulkan-1.dll")}}) {
        void* h = dlopen(so, RTLD_LAZY | RTLD_LOCAL);
        r.push_back({k, h != nullptr, h ? std::string("loaded ") + so : std::string("runtime not found: ") + so});
        if (h) dlclose(h);
    }
    r.push_back({backend_kind::metal, false, "metal backend not built on this platform"});
    return r;
}

template <class T> class buffer {
public:
    buffer() = default;
    explicit buffer(std::size_t n) : d_(std::make_shared<std::vector<T>>(n)) {}
    std::size_t size() const { return d_ ? d_->size() : 0; }
    span<T> view() const { return {d_->data(), d_->size()}; }
    T* data() const { return d_->data(); }
private:
    std::shared_ptr<std::vector<T>> d_;
};

class device;
class executable;

class graph {
public:
    struct node_ref {
        graph* g; std::size_t id;
        node_ref& after(node_ref o) { g->nodes_[id].deps.push_back(o.id); return *this; }
    };
    template <class T> node_ref copy(span<const T> src, buffer<T> dst) {
        return add([src, dst] { std::copy(src.begin(), src.end(), dst.data()); }, "copy");
    }
    template <class T> node_ref copy(buffer<T> src, span<T> dst) {
        return add([src, dst] { std::copy(src.data(), src.data() + src.size(), dst.begin()); }, "copy");
    }
    template <class T> node_ref copy(span<T> src, buffer<T> dst) { return copy(span<const T>(src), dst); }
    // Launch a kernel functor over an extent; work is split across the task pool by rows of x.
    template <class K, class... Args> node_ref launch(K k, extent e, Args... args) {
        return add([k, e, args...] {
            i64 total = e.total();
            task::parallel_for(std::size_t(total), 4096, [&](std::size_t b, std::size_t en) {
                for (std::size_t f = b; f < en; ++f) {
                    item<3> it;
                    i64 x = i64(f) % e.n[0], y = (i64(f) / e.n[0]) % e.n[1], z = i64(f) / (e.n[0] * e.n[1]);
                    it.gid = {x, y, z}; it.ext = e.n; it.lid = it.gid; it.grp = {0, 0, 0};
                    k(it, as_span(args)...);
                }
            });
        }, "launch");
    }
    node_ref host(std::function<void()> f) { return add(std::move(f), "host"); }
    executable compile(const device& d);
    std::size_t size() const { return nodes_.size(); }
private:
    template <class T> static span<T> as_span(const buffer<T>& b) { return b.view(); }
    template <class T> static const T& as_span(const T& v) { return v; }
    struct node { std::function<void()> fn; std::vector<std::size_t> deps; const char* kind; };
    node_ref add(std::function<void()> f, const char* kind) {
        std::size_t id = nodes_.size();
        nodes_.push_back({std::move(f), {}, kind});
        if (id) nodes_[id].deps.push_back(id - 1); // default: in-order queue semantics; .after adds more
        return {this, id};
    }
    std::vector<node> nodes_;
    friend class executable;
};

// Compiled, replayable form: dependency order resolved once, replay is a flat loop.
class executable {
public:
    void run() const { for (auto* f : order_) (*f)(); }
    std::size_t steps() const { return order_.size(); }
private:
    friend class graph;
    std::vector<const std::function<void()>*> order_;
    std::shared_ptr<std::vector<graph::node>> keep_;
};

class queue {
public:
    explicit queue(const device* d) : d_(d) {}
    const device& dev() const { return *d_; }
private:
    const device* d_;
};

class device {
public:
    explicit device(capabilities c) : caps_(std::move(c)) {}
    const capabilities& caps() const { return caps_; }
    template <class T> buffer<T> alloc(std::size_t n) const { return buffer<T>(n); }
    queue make_queue() const { return queue(this); }
private:
    capabilities caps_;
};

inline executable graph::compile(const device&) {
    executable e;
    e.keep_ = std::make_shared<std::vector<node>>(std::move(nodes_));
    auto& ns = *e.keep_;
    std::vector<int> state(ns.size(), 0);
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (state[i] == 2) return;
        if (state[i] == 1) raise(errc::invalid_argument, "grid graph has a cycle");
        state[i] = 1;
        for (auto d : ns[i].deps) visit(d);
        state[i] = 2;
        e.order_.push_back(&ns[i].fn);
    };
    for (std::size_t i = 0; i < ns.size(); ++i) visit(i);
    nodes_.clear();
    return e;
}

enum class prefer { any, discrete, cpu };

inline std::vector<device> all() {
    capabilities c;
    c.kind = backend_kind::cpu;
    c.name = std::string("cpu/") + simd::active().target;
    c.compute_units = int(task::pool::global().size());
    c.subgroup_width = std::string_view(simd::active().target) == "avx512" ? 16 : (std::string_view(simd::active().target) == "avx2" ? 8 : 4);
    c.fp16 = true; c.bf16 = true;
    c.score = c.compute_units * c.subgroup_width;
    return {device(c)};
}
inline device select(prefer = prefer::any) { return all().front(); }

} // namespace forge::gpu
