#pragma once
// Image helpers on HWC float images: bilinear resize (half-pixel centres, as OpenCV/PIL), RGB<->YUV (BT.601
// full range) and greedy non-maximum suppression.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace forge::vision {
struct image { std::size_t h = 0, w = 0, c = 0; std::vector<float> px; float& at(std::size_t y, std::size_t x, std::size_t k) { return px[(y * w + x) * c + k]; } float at(std::size_t y, std::size_t x, std::size_t k) const { return px[(y * w + x) * c + k]; } };
inline image resize_bilinear(const image& s, std::size_t H, std::size_t W) {
    image d{H, W, s.c, std::vector<float>(H * W * s.c)};
    double sy = double(s.h) / double(H), sx = double(s.w) / double(W);
    for (std::size_t y = 0; y < H; ++y) {
        double fy = std::clamp((double(y) + 0.5) * sy - 0.5, 0.0, double(s.h - 1)); std::size_t y0 = std::size_t(fy), y1 = std::min(y0 + 1, s.h - 1); double ty = fy - double(y0);
        for (std::size_t x = 0; x < W; ++x) {
            double fx = std::clamp((double(x) + 0.5) * sx - 0.5, 0.0, double(s.w - 1)); std::size_t x0 = std::size_t(fx), x1 = std::min(x0 + 1, s.w - 1); double tx = fx - double(x0);
            for (std::size_t k = 0; k < s.c; ++k)
                d.at(y, x, k) = float((1 - ty) * ((1 - tx) * s.at(y0, x0, k) + tx * s.at(y0, x1, k)) + ty * ((1 - tx) * s.at(y1, x0, k) + tx * s.at(y1, x1, k)));
        }
    }
    return d;
}
inline void rgb_to_yuv(const float* rgb, float* yuv, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { float r = rgb[3 * i], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        yuv[3 * i] = 0.299f * r + 0.587f * g + 0.114f * b; yuv[3 * i + 1] = -0.168736f * r - 0.331264f * g + 0.5f * b; yuv[3 * i + 2] = 0.5f * r - 0.418688f * g - 0.081312f * b; }
}
inline void yuv_to_rgb(const float* yuv, float* rgb, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { float y = yuv[3 * i], u = yuv[3 * i + 1], v = yuv[3 * i + 2];
        rgb[3 * i] = y + 1.402f * v; rgb[3 * i + 1] = y - 0.344136f * u - 0.714136f * v; rgb[3 * i + 2] = y + 1.772f * u; }
}
struct box { float x0, y0, x1, y1; };
inline float iou(const box& a, const box& b) {
    float iw = std::max(0.f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0)), ih = std::max(0.f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0)), in = iw * ih;
    float u = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - in; return u > 0 ? in / u : 0.f;
}
// Indices kept, highest score first.
inline std::vector<std::size_t> nms(const std::vector<box>& b, const std::vector<float>& score, float thr) {
    std::vector<std::size_t> o(b.size()); std::iota(o.begin(), o.end(), 0);
    std::stable_sort(o.begin(), o.end(), [&](auto i, auto j) { return score[i] > score[j]; });
    std::vector<std::size_t> keep; std::vector<char> dead(b.size());
    for (auto i : o) { if (dead[i]) continue; keep.push_back(i); for (auto j : o) if (!dead[j] && j != i && iou(b[i], b[j]) > thr) dead[j] = 1; }
    return keep;
}
// ROI-Align (Mask R-CNN): average of `samples`^2 bilinear samples per output bin; box in input pixel coords.
inline image roi_align(const image& s, const box& b, std::size_t ph, std::size_t pw, int samples = 2) {
    image o{ph, pw, s.c, std::vector<float>(ph * pw * s.c)};
    float bh = (b.y1 - b.y0) / float(ph), bw = (b.x1 - b.x0) / float(pw);
    auto bil = [&](float y, float x, std::size_t k) {
        if (y < -1 || y > float(s.h) || x < -1 || x > float(s.w)) return 0.f;
        y = std::max(0.f, y); x = std::max(0.f, x);
        std::size_t y0 = std::min(std::size_t(y), s.h - 1), x0 = std::min(std::size_t(x), s.w - 1);
        std::size_t y1 = std::min(y0 + 1, s.h - 1), x1 = std::min(x0 + 1, s.w - 1);
        float ly = y0 == s.h - 1 ? 0.f : y - float(y0), lx = x0 == s.w - 1 ? 0.f : x - float(x0);
        return (1 - ly) * ((1 - lx) * s.at(y0, x0, k) + lx * s.at(y0, x1, k)) + ly * ((1 - lx) * s.at(y1, x0, k) + lx * s.at(y1, x1, k));
    };
    for (std::size_t i = 0; i < ph; ++i) for (std::size_t j = 0; j < pw; ++j) for (std::size_t k = 0; k < s.c; ++k) {
        float acc = 0;
        for (int a = 0; a < samples; ++a) for (int c = 0; c < samples; ++c)
            acc += bil(b.y0 + bh * (float(i) + (float(a) + .5f) / float(samples)), b.x0 + bw * (float(j) + (float(c) + .5f) / float(samples)), k);
        o.at(i, j, k) = acc / float(samples * samples);
    }
    return o;
}
}
