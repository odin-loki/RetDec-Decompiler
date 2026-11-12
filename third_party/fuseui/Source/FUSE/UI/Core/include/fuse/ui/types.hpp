// FUSE UI core value types (logical pixels, straight-alpha colours). Plan §4.4.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace fuse::ui {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;

struct Vec2 {
    f32 x = 0.f, y = 0.f;
    friend bool operator==(const Vec2&, const Vec2&) = default;
};

struct Size {
    f32 w = 0.f, h = 0.f;
    friend bool operator==(const Size&, const Size&) = default;
};

struct Rect {
    f32 x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    f32 right() const { return x + w; }
    f32 bottom() const { return y + h; }
    bool empty() const { return w <= 0.f || h <= 0.f; }
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    Rect intersect(const Rect& o) const {
        const f32 x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const f32 x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const f32 x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        return {x0, y0, std::max(right(), o.right()) - x0, std::max(bottom(), o.bottom()) - y0};
    }
    friend bool operator==(const Rect&, const Rect&) = default;
};

struct Edges {
    f32 l = 0.f, t = 0.f, r = 0.f, b = 0.f;
    f32 h() const { return l + r; }
    f32 v() const { return t + b; }
    friend bool operator==(const Edges&, const Edges&) = default;
};

/// Straight-alpha linear-in-display-space colour, 0..1. Premultiplied at rasterisation (matches frame.ui_composite).
struct Color {
    f32 r = 0.f, g = 0.f, b = 0.f, a = 0.f;
    static constexpr Color rgb(u32 hex, f32 alpha = 1.f) {
        return {f32((hex >> 16) & 0xFF) / 255.f, f32((hex >> 8) & 0xFF) / 255.f, f32(hex & 0xFF) / 255.f, alpha};
    }
    Color withAlpha(f32 alpha) const { return {r, g, b, alpha}; }
    bool transparent() const { return a <= 0.f; }
    friend bool operator==(const Color&, const Color&) = default;
};

/// FNV-1a 32 (0 is never returned); ids, class names and immediate-mode keys.
constexpr u32 hashId(std::string_view s, u32 seed = 2166136261u) {
    u32 h = seed;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

} // namespace fuse::ui
