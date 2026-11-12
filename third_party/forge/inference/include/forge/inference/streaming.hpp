#pragma once
// Session kind `streaming`: lock-free SPSC ring buffer + frame scheduler (window/hop framing over an arbitrary
// chunked input) + latency budget. When the consumer falls behind by more than the budget, the oldest pending
// frames are dropped (counted in stats) so output latency stays bounded — the policy live audio/video wants.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <vector>

namespace forge::inference::streaming {

template <class T> class ring {                         // single producer, single consumer
public:
    explicit ring(std::size_t cap) : buf_(round_up(cap + 1)), mask_(buf_.size() - 1) {}
    std::size_t capacity() const { return buf_.size() - 1; }
    std::size_t size() const { return (w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)) & mask_; }
    std::size_t push(const T* p, std::size_t n) {         // returns how many were written
        std::size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
        std::size_t room = capacity() - ((w - r) & mask_), k = n < room ? n : room;
        for (std::size_t i = 0; i < k; ++i) buf_[(w + i) & mask_] = p[i];
        w_.store((w + k) & mask_, std::memory_order_release); return k;
    }
    std::size_t peek(T* p, std::size_t n, std::size_t off = 0) const {
        std::size_t r = r_.load(std::memory_order_relaxed), have = size();
        if (off >= have) return 0;
        std::size_t k = n < have - off ? n : have - off;
        for (std::size_t i = 0; i < k; ++i) p[i] = buf_[(r + off + i) & mask_];
        return k;
    }
    void consume(std::size_t n) { std::size_t r = r_.load(std::memory_order_relaxed); r_.store((r + n) & mask_, std::memory_order_release); }
private:
    static std::size_t round_up(std::size_t n) { std::size_t p = 1; while (p < n) p <<= 1; return p; }
    std::vector<T> buf_; std::size_t mask_; std::atomic<std::size_t> r_{0}, w_{0};
};

struct options {
    std::size_t window = 400, hop = 160;                 // frame length / stride in samples
    std::size_t max_lag_frames = 8;                      // latency budget in frames pending
    std::chrono::microseconds deadline{0};               // per-frame processing budget (0 = none); overruns counted
    std::size_t capacity = 1 << 16;
};
struct stats { std::size_t frames = 0, dropped = 0, overruns = 0, samples_in = 0, samples_rejected = 0; double worst_ms = 0; };

class session {
public:
    using frame_fn = std::function<void(const float* frame, std::size_t len, std::size_t index)>;
    session(options o, frame_fn f) : o_(o), q_(o.capacity), f_(std::move(f)), frame_(o.window) {
        if (!o.window || !o.hop) throw std::invalid_argument("streaming: window and hop must be > 0");
    }
    // Producer side (any chunk size). Samples that do not fit are rejected and counted.
    std::size_t feed(const float* p, std::size_t n) { std::size_t k = q_.push(p, n); st_.samples_in += k; st_.samples_rejected += n - k; return k; }
    std::size_t pending_frames() const { std::size_t s = q_.size(); return s < o_.window ? 0 : (s - o_.window) / o_.hop + 1; }
    // Consumer side: process ready frames, enforcing the latency budget first. Returns frames processed.
    std::size_t pump() {
        std::size_t done = 0;
        if (std::size_t p = pending_frames(); p > o_.max_lag_frames) {
            std::size_t drop = p - o_.max_lag_frames; q_.consume(drop * o_.hop); st_.dropped += drop; next_ += drop;
        }
        while (q_.size() >= o_.window) {
            q_.peek(frame_.data(), o_.window);
            auto t0 = std::chrono::steady_clock::now();
            f_(frame_.data(), o_.window, next_++);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > st_.worst_ms) st_.worst_ms = ms;
            if (o_.deadline.count() && ms * 1000 > double(o_.deadline.count())) ++st_.overruns;
            q_.consume(o_.hop); ++st_.frames; ++done;
        }
        return done;
    }
    const stats& stat() const { return st_; }
private:
    options o_; ring<float> q_; frame_fn f_; std::vector<float> frame_; stats st_; std::size_t next_ = 0;
};

} // namespace forge::inference::streaming
