#pragma once
// Portable BLAKE3 (hash mode, 32-byte output). Used for .fpk section/tensor integrity.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace forge::hash {

class blake3 {
public:
    using digest = std::array<std::uint8_t, 32>;
    blake3() { reset_chunk(0); }
    void update(const void* data, std::size_t n) {
        auto* in = static_cast<const std::uint8_t*>(data);
        while (n) {
            if (chunk_len() == 1024) {
                auto cv = chunk_output().chain();
                std::uint64_t total = counter_ + 1;
                add_cv(cv, total);
                reset_chunk(total);
            }
            std::size_t take = std::min<std::size_t>(1024 - chunk_len(), n);
            chunk_update(in, take); in += take; n -= take;
        }
    }
    digest final() const {
        output o = chunk_output();
        for (std::size_t r = stack_.size(); r > 0; --r) o = parent(stack_[r - 1], o.chain());
        auto w = o.root_words();
        digest d;
        for (int i = 0; i < 8; ++i) for (int b = 0; b < 4; ++b) d[std::size_t(4 * i + b)] = std::uint8_t(w[std::size_t(i)] >> (8 * b));
        return d;
    }
    static digest of(std::span<const std::uint8_t> s) { blake3 h; h.update(s.data(), s.size()); return h.final(); }
    static std::string hex(const digest& d) {
        static const char* x = "0123456789abcdef"; std::string s;
        for (auto b : d) { s += x[b >> 4]; s += x[b & 15]; }
        return s;
    }

private:
    using cvt = std::array<std::uint32_t, 8>;
    static constexpr cvt IV = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    enum : std::uint32_t { CHUNK_START = 1, CHUNK_END = 2, PARENT = 4, ROOT = 8 };
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    static void g(std::uint32_t* s, int a, int b, int c, int d, std::uint32_t x, std::uint32_t y) {
        s[a] = s[a] + s[b] + x; s[d] = rotr(s[d] ^ s[a], 16); s[c] = s[c] + s[d]; s[b] = rotr(s[b] ^ s[c], 12);
        s[a] = s[a] + s[b] + y; s[d] = rotr(s[d] ^ s[a], 8);  s[c] = s[c] + s[d]; s[b] = rotr(s[b] ^ s[c], 7);
    }
    static std::array<std::uint32_t, 16> compress(const cvt& cv, const std::uint32_t* blk, std::uint64_t ctr, std::uint32_t len, std::uint32_t flags) {
        static constexpr int P[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};
        std::uint32_t s[16] = {cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7], IV[0], IV[1], IV[2], IV[3],
                               std::uint32_t(ctr), std::uint32_t(ctr >> 32), len, flags};
        std::uint32_t m[16]; std::memcpy(m, blk, 64);
        for (int r = 0; r < 7; ++r) {
            g(s, 0, 4, 8, 12, m[0], m[1]); g(s, 1, 5, 9, 13, m[2], m[3]); g(s, 2, 6, 10, 14, m[4], m[5]); g(s, 3, 7, 11, 15, m[6], m[7]);
            g(s, 0, 5, 10, 15, m[8], m[9]); g(s, 1, 6, 11, 12, m[10], m[11]); g(s, 2, 7, 8, 13, m[12], m[13]); g(s, 3, 4, 9, 14, m[14], m[15]);
            if (r < 6) { std::uint32_t t[16]; for (int i = 0; i < 16; ++i) t[i] = m[P[i]]; std::memcpy(m, t, 64); }
        }
        std::array<std::uint32_t, 16> o;
        for (int i = 0; i < 8; ++i) { o[std::size_t(i)] = s[i] ^ s[i + 8]; o[std::size_t(i + 8)] = s[i + 8] ^ cv[std::size_t(i)]; }
        return o;
    }
    static void words(const std::uint8_t* b, std::uint32_t* w) {
        for (int i = 0; i < 16; ++i) w[i] = std::uint32_t(b[4 * i]) | std::uint32_t(b[4 * i + 1]) << 8 | std::uint32_t(b[4 * i + 2]) << 16 | std::uint32_t(b[4 * i + 3]) << 24;
    }
    struct output {
        cvt key; std::uint32_t blk[16]; std::uint64_t ctr; std::uint32_t len, flags;
        cvt chain() const { auto o = compress(key, blk, ctr, len, flags); cvt r; std::copy_n(o.begin(), 8, r.begin()); return r; }
        std::array<std::uint32_t, 16> root_words() const { return compress(key, blk, 0, len, flags | ROOT); }
    };
    static output parent(const cvt& l, const cvt& r) {
        output o{IV, {}, 0, 64, PARENT};
        std::copy(l.begin(), l.end(), o.blk); std::copy(r.begin(), r.end(), o.blk + 8);
        return o;
    }
    void add_cv(cvt cv, std::uint64_t total) {
        while ((total & 1) == 0) { cv = parent(stack_.back(), cv).chain(); stack_.pop_back(); total >>= 1; }
        stack_.push_back(cv);
    }
    void reset_chunk(std::uint64_t c) { ccv_ = IV; counter_ = c; blocks_ = 0; buf_len_ = 0; }
    std::size_t chunk_len() const { return 64 * blocks_ + buf_len_; }
    std::uint32_t start_flag() const { return blocks_ == 0 ? CHUNK_START : 0; }
    void chunk_update(const std::uint8_t* in, std::size_t n) {
        while (n) {
            if (buf_len_ == 64) {
                std::uint32_t w[16]; words(buf_, w);
                auto o = compress(ccv_, w, counter_, 64, start_flag());
                std::copy_n(o.begin(), 8, ccv_.begin());
                ++blocks_; buf_len_ = 0;
            }
            std::size_t t = std::min<std::size_t>(64 - buf_len_, n);
            std::memcpy(buf_ + buf_len_, in, t); buf_len_ += t; in += t; n -= t;
        }
    }
    output chunk_output() const {
        output o{ccv_, {}, counter_, std::uint32_t(buf_len_), start_flag() | CHUNK_END};
        std::uint8_t b[64] = {}; std::memcpy(b, buf_, buf_len_); words(b, o.blk);
        return o;
    }
    cvt ccv_; std::uint64_t counter_ = 0; std::size_t blocks_ = 0, buf_len_ = 0; std::uint8_t buf_[64] = {};
    std::vector<cvt> stack_;
};

} // namespace forge::hash
