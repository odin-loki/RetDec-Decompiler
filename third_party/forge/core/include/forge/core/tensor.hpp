#pragma once
// Minimal owning/non-owning dense tensor used across graph/inference (row-major, f32 compute type).
#include <forge/core/dtypes.hpp>
#include <forge/core/error.hpp>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

namespace forge {

using shape_t = std::vector<i64>;

inline i64 numel(const shape_t& s) {
    return std::accumulate(s.begin(), s.end(), i64{1}, std::multiplies<>{});
}
inline std::string shape_str(const shape_t& s) {
    std::string r = "[";
    for (std::size_t i = 0; i < s.size(); ++i) r += (i ? "," : "") + std::to_string(s[i]);
    return r + "]";
}

struct tensor_view {
    float* data = nullptr;
    shape_t shape;
    // Quantised weight payload (data == nullptr): encoded rows read in place by quantised kernels.
    const std::uint8_t* qdata = nullptr;
    const void* qcodec = nullptr;   // const graph::codec*
    std::size_t size() const { return std::size_t(numel(shape)); }
    std::span<float> span() const { return {data, size()}; }
};

struct tensor {
    shape_t shape;
    std::vector<float> data;
    tensor() = default;
    explicit tensor(shape_t s, float fill = 0.f) : shape(std::move(s)), data(std::size_t(numel(shape)), fill) {}
    tensor_view view() { return {data.data(), shape}; }
    std::size_t size() const { return data.size(); }
};

} // namespace forge
