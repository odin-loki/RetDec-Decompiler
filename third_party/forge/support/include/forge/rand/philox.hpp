#pragma once
// Counter-based Philox4x32-10: identical streams on every backend (pure function of key+counter).
#include <array>
#include <cstdint>

namespace forge::rand {
struct philox4x32 {
    std::array<std::uint32_t, 2> key;
    static constexpr std::array<std::uint32_t, 4> block(std::array<std::uint32_t, 4> c, std::array<std::uint32_t, 2> k) {
        for (int r = 0; r < 10; ++r) {
            std::uint64_t p0 = std::uint64_t(0xD2511F53u) * c[0], p1 = std::uint64_t(0xCD9E8D57u) * c[2];
            c = {std::uint32_t(p1 >> 32) ^ c[1] ^ k[0], std::uint32_t(p1), std::uint32_t(p0 >> 32) ^ c[3] ^ k[1], std::uint32_t(p0)};
            k[0] += 0x9E3779B9u; k[1] += 0xBB67AE85u;
        }
        return c;
    }
    constexpr std::array<std::uint32_t, 4> operator()(std::uint64_t counter, std::uint64_t stream = 0) const {
        return block({std::uint32_t(counter), std::uint32_t(counter >> 32), std::uint32_t(stream), std::uint32_t(stream >> 32)}, key);
    }
    static constexpr float to_unit(std::uint32_t x) { return float(x >> 8) * (1.0f / 16777216.0f); }
};
// Threefry4x32-20 (Random123): the second counter-based family, same (counter, stream) interface as philox4x32.
struct threefry4x32 {
    std::array<std::uint32_t, 4> key;
    static constexpr std::uint32_t rotl(std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
    static constexpr std::array<std::uint32_t, 4> block(std::array<std::uint32_t, 4> x, std::array<std::uint32_t, 4> k4) {
        constexpr int R[8][2] = {{10, 26}, {11, 21}, {13, 27}, {23, 5}, {6, 20}, {17, 11}, {25, 10}, {18, 20}};
        std::uint32_t ks[5] = {k4[0], k4[1], k4[2], k4[3], 0x1BD11BDAu ^ k4[0] ^ k4[1] ^ k4[2] ^ k4[3]};
        for (int i = 0; i < 4; ++i) x[std::size_t(i)] += ks[i];
        for (int r = 0; r < 20; ++r) {
            const int* rc = R[r % 8];
            if (r % 2 == 0) { x[0] += x[1]; x[1] = rotl(x[1], rc[0]) ^ x[0]; x[2] += x[3]; x[3] = rotl(x[3], rc[1]) ^ x[2]; }
            else            { x[0] += x[3]; x[3] = rotl(x[3], rc[0]) ^ x[0]; x[2] += x[1]; x[1] = rotl(x[1], rc[1]) ^ x[2]; }
            if (r % 4 == 3) {
                int s = (r + 1) / 4;
                for (int i = 0; i < 4; ++i) x[std::size_t(i)] += ks[(s + i) % 5];
                x[3] += std::uint32_t(s);
            }
        }
        return x;
    }
    constexpr std::array<std::uint32_t, 4> operator()(std::uint64_t counter, std::uint64_t stream = 0) const {
        return block({std::uint32_t(counter), std::uint32_t(counter >> 32), std::uint32_t(stream), std::uint32_t(stream >> 32)}, key);
    }
};
} // namespace forge::rand
