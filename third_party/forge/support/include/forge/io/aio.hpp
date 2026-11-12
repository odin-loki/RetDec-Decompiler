#pragma once
// io: asynchronous file reads. Linux uses io_uring through raw syscalls (no liburing dependency); anything
// else, or a kernel/sandbox that refuses io_uring, falls back to a small pread thread pool with the same
// interface. Short reads are resubmitted internally, so a completion always covers the whole request
// (or reports an error / EOF).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <forge/os/os.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#if defined(_WIN32)
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#else
#include <unistd.h>
#endif
#include <vector>
#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define FORGE_IO_URING 1
#endif

namespace forge::io {

struct completion { std::uint64_t user; long result; };   // result: bytes read, or -errno

class async_reader {
public:
    enum class backend { io_uring, threads };
    explicit async_reader(unsigned depth = 64, bool allow_uring = true) : depth_(depth) {
#ifdef FORGE_IO_URING
        if (allow_uring && init_uring()) { be_ = backend::io_uring; return; }
#endif
        (void)allow_uring;
        be_ = backend::threads;
        for (unsigned i = 0; i < 4; ++i) pool_.emplace_back([this] { worker(); });
    }
    ~async_reader() {
        { std::lock_guard lk(mu_); quit_ = true; } cv_.notify_all();
        for (auto& t : pool_) t.join();
#ifdef FORGE_IO_URING
        if (ring_fd_ >= 0) {
            if (sq_ptr_) munmap(sq_ptr_, sq_sz_);
            if (cq_ptr_ && cq_ptr_ != sq_ptr_) munmap(cq_ptr_, cq_sz_);
            if (sqes_) munmap(sqes_, sqes_sz_);
            close(ring_fd_);
        }
#endif
    }
    backend kind() const { return be_; }
    // Queue a read of `len` bytes at `off` into `buf`. Never blocks for I/O (may wait for a free slot).
    void read(int fd, std::uint64_t off, void* buf, std::size_t len, std::uint64_t user) {
        op o{fd, off, static_cast<char*>(buf), len, 0, user};
        ++inflight_;
        if (be_ == backend::threads) { { std::lock_guard lk(mu_); q_.push_back(o); } cv_.notify_one(); return; }
#ifdef FORGE_IO_URING
        ops_.push_back(o); pump();
#endif
    }
    std::size_t inflight() const { return inflight_; }
    // Wait for at least `min` completions (0 = poll) and return them.
    std::vector<completion> wait(std::size_t min = 1) {
        std::vector<completion> out;
        if (be_ == backend::threads) {
            std::unique_lock lk(mu_);
            done_cv_.wait(lk, [&] { return done_.size() >= std::min(min, inflight_.load()); });
            out.assign(done_.begin(), done_.end()); done_.clear();
            inflight_ -= out.size();
            return out;
        }
#ifdef FORGE_IO_URING
        while (out.size() < min && inflight_ > 0) { reap(out, true); }
        reap(out, false);
        inflight_ -= out.size();
#endif
        return out;
    }
    // Convenience: read a whole file with `chunk`-sized requests in flight together.
    std::vector<std::uint8_t> read_file(const std::string& path, std::size_t chunk = 1 << 20) {
#if defined(_WIN32)   // the public API takes CRT fds; size via forge::os (UTF-8 path aware)
        int fd = ::_wopen(os::widen(path).c_str(), O_RDONLY | O_BINARY);
        if (fd < 0) throw std::runtime_error("io: cannot open " + path);
        auto sz = os::stat(path).size;
        struct closer { int fd; ~closer() { ::_close(fd); } } guard{fd};
#else
        auto f = os::file::open(path, os::rd);
        if (!f) throw std::runtime_error("io: cannot open " + path + ": " + f.err.what);
        int fd = f->native(); auto sz = f->size().value;
#endif
        std::vector<std::uint8_t> data(static_cast<std::size_t>(sz));
        std::size_t n = 0;
        for (std::size_t off = 0; off < data.size(); off += chunk, ++n) read(fd, off, data.data() + off, std::min(chunk, data.size() - off), off);
        std::size_t got = 0; long err = 0;
        while (got < n) for (auto& c : wait(1)) { ++got; if (c.result < 0) err = c.result; }
        if (err) throw std::runtime_error("io: read failed (" + std::to_string(-err) + ")");
        return data;
    }

private:
    static long long pread_at(int fd, char* b, std::size_t n, std::uint64_t off) {
#if defined(_WIN32)   // positional read on the CRT fd's handle (no shared file pointer)
        OVERLAPPED ov{}; ov.Offset = DWORD(off); ov.OffsetHigh = DWORD(off >> 32); DWORD got = 0;
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (!ReadFile(h, b, DWORD(std::min<std::size_t>(n, 1u << 30)), &got, &ov)) { if (GetLastError() == ERROR_HANDLE_EOF) return 0; errno = EIO; return -1; }
        return got;
#else
        return ::pread(fd, b, n, off_t(off));
#endif
    }
    struct op { int fd; std::uint64_t off; char* buf; std::size_t len, done; std::uint64_t user; };
    void worker() {
        for (;;) {
            op o;
            { std::unique_lock lk(mu_); cv_.wait(lk, [&] { return quit_ || !q_.empty(); }); if (quit_ && q_.empty()) return; o = q_.front(); q_.pop_front(); }
            long r = 0;
            while (o.done < o.len) {
                auto k = pread_at(o.fd, o.buf + o.done, o.len - o.done, o.off + o.done);
                if (k < 0) { if (errno == EINTR) continue; r = -errno; break; }
                if (k == 0) break;
                o.done += std::size_t(k);
            }
            { std::lock_guard lk(mu_); done_.push_back({o.user, r < 0 ? r : long(o.done)}); }
            done_cv_.notify_all();
        }
    }
#ifdef FORGE_IO_URING
    bool init_uring() {
        io_uring_params p{};
        int fd = int(syscall(SYS_io_uring_setup, depth_, &p));
        if (fd < 0) return false;
        ring_fd_ = fd;
        sq_sz_ = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        cq_sz_ = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        bool single = p.features & IORING_FEAT_SINGLE_MMAP;
        if (single) sq_sz_ = cq_sz_ = std::max(sq_sz_, cq_sz_);
        sq_ptr_ = mmap(nullptr, sq_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
        if (sq_ptr_ == MAP_FAILED) { sq_ptr_ = nullptr; return false; }
        cq_ptr_ = single ? sq_ptr_ : mmap(nullptr, cq_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        if (cq_ptr_ == MAP_FAILED) { cq_ptr_ = nullptr; return false; }
        sqes_sz_ = p.sq_entries * sizeof(io_uring_sqe);
        sqes_ = static_cast<io_uring_sqe*>(mmap(nullptr, sqes_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES));
        if (sqes_ == MAP_FAILED) { sqes_ = nullptr; return false; }
        auto* sq = static_cast<char*>(sq_ptr_); auto* cq = static_cast<char*>(cq_ptr_);
        sq_head_ = reinterpret_cast<unsigned*>(sq + p.sq_off.head); sq_tail_ = reinterpret_cast<unsigned*>(sq + p.sq_off.tail);
        sq_mask_ = *reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask); sq_array_ = reinterpret_cast<unsigned*>(sq + p.sq_off.array);
        cq_head_ = reinterpret_cast<unsigned*>(cq + p.cq_off.head); cq_tail_ = reinterpret_cast<unsigned*>(cq + p.cq_off.tail);
        cq_mask_ = *reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask); cqes_ = reinterpret_cast<io_uring_cqe*>(cq + p.cq_off.cqes);
        sq_entries_ = p.sq_entries;
        return true;
    }
    // Move queued ops into the SQ (as many as fit / are allowed in flight) and submit.
    void pump() {
        unsigned n = 0;
        while (!ops_.empty() && active_.size() < sq_entries_) {
            unsigned tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED), head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
            if (tail - head >= sq_entries_) break;
            op o = ops_.front(); ops_.pop_front();
            std::size_t slot = alloc_slot(o);
            unsigned i = tail & sq_mask_; io_uring_sqe& e = sqes_[i]; std::memset(&e, 0, sizeof e);
            e.opcode = IORING_OP_READ; e.fd = o.fd; e.off = o.off + o.done; e.addr = reinterpret_cast<std::uint64_t>(o.buf + o.done);
            e.len = unsigned(std::min<std::size_t>(o.len - o.done, 1u << 30)); e.user_data = slot;
            sq_array_[i] = i;
            __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE); ++n;
        }
        if (n && syscall(SYS_io_uring_enter, ring_fd_, n, 0, 0, nullptr, 0) < 0) throw std::runtime_error("io_uring_enter failed");
    }
    std::size_t alloc_slot(const op& o) {
        if (!free_.empty()) { auto s = free_.back(); free_.pop_back(); slots_[s] = o; active_.push_back(s); return s; }
        slots_.push_back(o); active_.push_back(slots_.size() - 1); return slots_.size() - 1;   // active_ used as count only
    }
    void reap(std::vector<completion>& out, bool block) {
        if (block && syscall(SYS_io_uring_enter, ring_fd_, 0, 1, IORING_ENTER_GETEVENTS, nullptr, 0) < 0 && errno != EINTR)
            throw std::runtime_error("io_uring_enter(wait) failed");
        unsigned head = __atomic_load_n(cq_head_, __ATOMIC_RELAXED);
        while (head != __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE)) {
            io_uring_cqe c = cqes_[head & cq_mask_]; ++head;
            __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
            auto slot = std::size_t(c.user_data); op& o = slots_[slot];
            bool fin = true; long res = 0;
            if (c.res < 0) res = c.res;
            else if (c.res == 0) res = long(o.done);                     // EOF
            else { o.done += std::size_t(c.res); if (o.done < o.len) { fin = false; ops_.push_front(o); } else res = long(o.done); }
            free_.push_back(slot); active_.pop_back();
            if (fin) out.push_back({o.user, res});
        }
        pump();
    }
    int ring_fd_ = -1; void *sq_ptr_ = nullptr, *cq_ptr_ = nullptr; io_uring_sqe* sqes_ = nullptr;
    std::size_t sq_sz_ = 0, cq_sz_ = 0, sqes_sz_ = 0; unsigned sq_entries_ = 0;
    unsigned *sq_head_ = nullptr, *sq_tail_ = nullptr, *sq_array_ = nullptr, *cq_head_ = nullptr, *cq_tail_ = nullptr;
    unsigned sq_mask_ = 0, cq_mask_ = 0; io_uring_cqe* cqes_ = nullptr;
    std::deque<op> ops_; std::vector<op> slots_; std::vector<std::size_t> free_, active_;
#endif
    unsigned depth_; backend be_ = backend::threads;
    std::atomic<std::size_t> inflight_{0};
    std::mutex mu_; std::condition_variable cv_, done_cv_; std::deque<op> q_; std::deque<completion> done_; bool quit_ = false;
    std::vector<std::thread> pool_;
};

} // namespace forge::io
