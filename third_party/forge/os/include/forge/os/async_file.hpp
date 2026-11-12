#pragma once
// Completion-based async file I/O with one API:
//   Linux: io_uring (raw syscalls, no liburing), falling back to a worker pool when the kernel lacks it or its
//   READ/WRITE opcodes (before 5.6; probed with IORING_REGISTER_PROBE) or a seccomp policy (e.g. Android app
//   sandbox) refuses it.  Windows: IOCP (overlapped ReOpenFile).  FreeBSD: POSIX AIO
//   with kqueue EVFILT_AIO completions.  macOS/iOS: POSIX AIO + aio_suspend.  Everywhere: FORGE_AIO=threads forces
//   the portable worker-pool backend.
#include <forge/os/detail.hpp>
#if defined(__linux__)
#include <linux/io_uring.h>
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
#include <aio.h>
#endif

namespace forge::os {
struct io_completion { std::uint64_t user = 0; std::int64_t result = 0; };   // result: bytes, or -errno/-GetLastError
enum class aio_backend { io_uring, iocp, kqueue_aio, posix_aio, threads };
inline const char* aio_backend_name(aio_backend b) {
    switch (b) { case aio_backend::io_uring: return "io_uring"; case aio_backend::iocp: return "iocp"; case aio_backend::kqueue_aio: return "kqueue+aio";
                 case aio_backend::posix_aio: return "posix_aio"; default: return "threads"; }
}

class async_file_io {
public:
    explicit async_file_io(unsigned depth = 64) : depth_(depth ? depth : 64) {
        bool force_threads = getenv("FORGE_AIO") == "threads";
#if defined(__linux__)
        if (!force_threads && uring_init_()) { be_ = aio_backend::io_uring; return; }
#elif defined(FORGE_OS_WINDOWS)
        if (!force_threads) { be_ = aio_backend::iocp; return; }
#elif defined(FORGE_OS_FREEBSD)
        if (!force_threads && (kq_ = ::kqueue()) >= 0) { be_ = aio_backend::kqueue_aio; return; }
#elif defined(__APPLE__)
        if (!force_threads) { be_ = aio_backend::posix_aio; return; }
#endif
        (void)force_threads; threads_init_();
    }
    ~async_file_io() {
#if defined(__linux__)
        if (ring_fd_ >= 0) { if (sq_ptr_) ::munmap(sq_ptr_, sq_sz_); if (cq_ptr_ && cq_ptr_ != sq_ptr_) ::munmap(cq_ptr_, cq_sz_); if (sqes_) ::munmap(sqes_, sqes_sz_); ::close(ring_fd_); }
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
        for (auto& p : aio_) { while (::aio_error(&p->cb) == EINPROGRESS) ::aio_suspend(reinterpret_cast<const struct aiocb* const*>(&p), 1, nullptr); ::aio_return(&p->cb); }
        if (kq_ >= 0) ::close(kq_);
#endif
        if (be_ == aio_backend::threads) {
            { std::lock_guard<std::mutex> g(tm_); stop_ = true; } tcv_.notify_all();
            for (auto& t : workers_) t.join();
        }
    }
    async_file_io(const async_file_io&) = delete; async_file_io& operator=(const async_file_io&) = delete;
    aio_backend backend() const { return be_; }
    const char* backend_name() const { return aio_backend_name(be_); }
    std::size_t in_flight() const { return inflight_; }

    status read(file& f, void* buf, std::size_t n, std::uint64_t off, std::uint64_t user) { return op_(f, buf, n, off, user, false); }
    status write(file& f, const void* buf, std::size_t n, std::uint64_t off, std::uint64_t user) { return op_(f, const_cast<void*>(buf), n, off, user, true); }
    // Release per-file state before closing f (Windows: the overlapped reopen of f; a no-op elsewhere).
    void forget(const file& f) {
#if defined(FORGE_OS_WINDOWS)
        cp_.forget(f);
#else
        (void)f;
#endif
    }

    // Wait for at least min_n completions (or timeout_ms; -1 = forever); returns what completed (possibly fewer on timeout).
    std::vector<io_completion> wait(std::size_t min_n = 1, int timeout_ms = -1) {
        std::vector<io_completion> out;
        std::uint64_t end = monotonic_ns() + std::uint64_t(timeout_ms < 0 ? 0 : timeout_ms) * 1000000ull;
        auto left_ms = [&]() -> int { if (timeout_ms < 0) return -1; auto now = monotonic_ns(); return now >= end ? 0 : int((end - now + 999999) / 1000000); };
        if (min_n > inflight_) min_n = inflight_;
        for (;;) {
            reap_(out);
            if (out.size() >= min_n) break;
            int ms = left_ms(); if (ms == 0) { reap_(out); break; }
            block_(ms);
        }
        inflight_ -= out.size(); return out;
    }
private:
    status op_(file& f, void* buf, std::size_t n, std::uint64_t off, std::uint64_t user, bool wr) {
        switch (be_) {
#if defined(__linux__)
        case aio_backend::io_uring: {
            if (inflight_ >= sq_entries_) { auto c = wait(1, -1); pending_.insert(pending_.end(), c.begin(), c.end()); }
            unsigned tail = *sq_tail_, idx = tail & *sq_mask_;
            io_uring_sqe* s = &sqes_[idx]; std::memset(s, 0, sizeof *s);
            s->opcode = wr ? IORING_OP_WRITE : IORING_OP_READ; s->fd = f.native(); s->off = off;
            // Linux moves at most 0x7ffff000 bytes per read/write: longer requests complete short, as with pread.
            s->addr = reinterpret_cast<std::uint64_t>(buf); s->len = unsigned(n < 0x7ffff000u ? n : 0x7ffff000u); s->user_data = user;
            sq_array_[idx] = idx;
            __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
            long r;
            do r = ::syscall(__NR_io_uring_enter, ring_fd_, 1u, 0u, 0u, nullptr, 0); while (r < 0 && errno == EINTR);
            if (r < 1 && __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) == tail) {
                // The kernel did not take the entry: withdraw it, or the next submission would carry it along
                // (an untracked I/O into a buffer the caller has since reused).
                auto e = r < 0 ? last_error("io_uring_enter") : error{EBUSY, "io_uring_enter: not submitted"};
                __atomic_store_n(sq_tail_, tail, __ATOMIC_RELEASE); return {{}, e};
            }
            ++inflight_; return {};
        }
#endif
#if defined(FORGE_OS_WINDOWS)
        case aio_backend::iocp: {
            std::uint64_t k = next_key_++; keys_[k] = user;
            bool ok = wr ? cp_.write_async(f, buf, n, off, k) : cp_.read_async(f, buf, n, off, k);
            if (!ok) { keys_.erase(k); return {{}, last_error(wr ? "WriteFile" : "ReadFile")}; }
            ++inflight_; return {};
        }
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
        case aio_backend::kqueue_aio: case aio_backend::posix_aio: {
            auto p = std::make_unique<aio_op>(); std::memset(&p->cb, 0, sizeof p->cb);
            p->cb.aio_fildes = f.native(); p->cb.aio_buf = buf; p->cb.aio_nbytes = n; p->cb.aio_offset = off_t(off); p->user = user;
#if defined(FORGE_OS_FREEBSD)
            if (be_ == aio_backend::kqueue_aio) { p->cb.aio_sigevent.sigev_notify = SIGEV_KEVENT; p->cb.aio_sigevent.sigev_notify_kqueue = kq_; p->cb.aio_sigevent.sigev_value.sival_ptr = p.get(); }
#endif
            if ((wr ? ::aio_write(&p->cb) : ::aio_read(&p->cb)) != 0) {
                if (errno == EAGAIN && !aio_.empty()) { auto c = wait(1, -1); pending_.insert(pending_.end(), c.begin(), c.end()); return op_(f, buf, n, off, user, wr); }
                return {{}, last_error(wr ? "aio_write" : "aio_read")};
            }
            aio_.push_back(std::move(p)); ++inflight_; return {};
        }
#endif
        default: {
            { std::lock_guard<std::mutex> g(tm_); tq_.push_back({f.native(), buf, n, off, user, wr}); }
            tcv_.notify_one(); ++inflight_; return {};
        }
        }
    }
    void reap_(std::vector<io_completion>& out) {
        if (!pending_.empty()) { out.insert(out.end(), pending_.begin(), pending_.end()); inflight_ += pending_.size(); pending_.clear(); }
        switch (be_) {
#if defined(__linux__)
        case aio_backend::io_uring: {
            unsigned head = *cq_head_, tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
            while (head != tail) { auto& c = cqes_[head & *cq_mask_]; out.push_back({c.user_data, c.res}); ++head; }
            __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE); return;
        }
#endif
#if defined(FORGE_OS_WINDOWS)
        case aio_backend::iocp: {
            completion c; while (cp_.wait(c, 0)) { auto it = keys_.find(c.key); std::uint64_t u = it == keys_.end() ? c.key : it->second; if (it != keys_.end()) keys_.erase(it);
                out.push_back({u, c.error ? -std::int64_t(c.error) : std::int64_t(c.bytes)}); }
            return;
        }
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
        case aio_backend::kqueue_aio: case aio_backend::posix_aio: {
            for (std::size_t i = 0; i < aio_.size();) {
                int e = ::aio_error(&aio_[i]->cb);
                if (e == EINPROGRESS) { ++i; continue; }
                auto r = ::aio_return(&aio_[i]->cb);
                out.push_back({aio_[i]->user, e ? -std::int64_t(e) : std::int64_t(r)});
                aio_.erase(aio_.begin() + std::ptrdiff_t(i));
            }
            return;
        }
#endif
        default: { std::lock_guard<std::mutex> g(tm_); out.insert(out.end(), tdone_.begin(), tdone_.end()); tdone_.clear(); return; }
        }
    }
    void block_(int ms) {
        switch (be_) {
#if defined(__linux__)
        case aio_backend::io_uring: { pollfd p{ring_fd_, POLLIN, 0}; ::poll(&p, 1, ms); return; }
#endif
#if defined(FORGE_OS_WINDOWS)
        case aio_backend::iocp: { completion c; if (cp_.wait(c, ms)) { auto it = keys_.find(c.key); std::uint64_t u = it == keys_.end() ? c.key : it->second; if (it != keys_.end()) keys_.erase(it);
                pending_.push_back({u, c.error ? -std::int64_t(c.error) : std::int64_t(c.bytes)}); --inflight_; } return; }
#endif
#if defined(FORGE_OS_FREEBSD)
        case aio_backend::kqueue_aio: { struct kevent ev; timespec ts{ms / 1000, (ms % 1000) * 1000000L}; ::kevent(kq_, nullptr, 0, &ev, 1, ms < 0 ? nullptr : &ts); return; }
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
        case aio_backend::posix_aio: {
            std::vector<const struct aiocb*> l; for (auto& p : aio_) l.push_back(&p->cb);
            if (l.empty()) return; timespec ts{ms / 1000, (ms % 1000) * 1000000L}; ::aio_suspend(l.data(), int(l.size()), ms < 0 ? nullptr : &ts); return;
        }
#endif
        default: { std::unique_lock<std::mutex> g(tm_); if (!tdone_.empty()) return;
            if (ms < 0) tcv_done_.wait(g, [&] { return !tdone_.empty(); }); else tcv_done_.wait_for(g, std::chrono::milliseconds(ms), [&] { return !tdone_.empty(); }); return; }
        }
    }
#if defined(__linux__)
    bool uring_init_() {
        io_uring_params p{}; int fd = int(::syscall(__NR_io_uring_setup, depth_, &p));
        if (fd < 0) return false;
        ring_fd_ = fd; sq_entries_ = p.sq_entries;
        // IORING_OP_READ/WRITE arrived in 5.6, together with IORING_REGISTER_PROBE; io_uring itself in 5.1. Without
        // a probe (5.1-5.5) or without both opcodes, use the worker pool. Layout from <linux/io_uring.h>, spelled out
        // so older uapi headers still build.
        struct probe_op { std::uint8_t op, resv; std::uint16_t flags; std::uint32_t resv2; };
        struct probe { std::uint8_t last_op, ops_len; std::uint16_t resv; std::uint32_t resv2[3]; probe_op ops[256]; };
        auto pr = std::make_unique<probe>(); std::memset(pr.get(), 0, sizeof *pr);
        if (::syscall(__NR_io_uring_register, fd, 8u /*IORING_REGISTER_PROBE*/, pr.get(), 256u) < 0) return uring_fail_();
        auto has = [&](unsigned op) { return op <= pr->last_op && op < pr->ops_len && (pr->ops[op].flags & 1u /*IO_URING_OP_SUPPORTED*/); };
        if (!has(IORING_OP_READ) || !has(IORING_OP_WRITE)) return uring_fail_();
        sq_sz_ = p.sq_off.array + p.sq_entries * sizeof(unsigned); cq_sz_ = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        bool single = p.features & IORING_FEAT_SINGLE_MMAP; if (single) sq_sz_ = cq_sz_ = std::max(sq_sz_, cq_sz_);
        sq_ptr_ = ::mmap(nullptr, sq_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
        if (sq_ptr_ == MAP_FAILED) { sq_ptr_ = nullptr; return uring_fail_(); }
        cq_ptr_ = single ? sq_ptr_ : ::mmap(nullptr, cq_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        if (cq_ptr_ == MAP_FAILED) { cq_ptr_ = nullptr; return uring_fail_(); }
        sqes_sz_ = p.sq_entries * sizeof(io_uring_sqe);
        void* s = ::mmap(nullptr, sqes_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
        if (s == MAP_FAILED) return uring_fail_();
        sqes_ = static_cast<io_uring_sqe*>(s);
        auto* sq = static_cast<std::uint8_t*>(sq_ptr_); auto* cq = static_cast<std::uint8_t*>(cq_ptr_);
        sq_head_ = reinterpret_cast<unsigned*>(sq + p.sq_off.head); sq_tail_ = reinterpret_cast<unsigned*>(sq + p.sq_off.tail); sq_mask_ = reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask); sq_array_ = reinterpret_cast<unsigned*>(sq + p.sq_off.array);
        cq_head_ = reinterpret_cast<unsigned*>(cq + p.cq_off.head); cq_tail_ = reinterpret_cast<unsigned*>(cq + p.cq_off.tail); cq_mask_ = reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>(cq + p.cq_off.cqes);
        return true;
    }
    bool uring_fail_() { if (sq_ptr_) ::munmap(sq_ptr_, sq_sz_); if (cq_ptr_ && cq_ptr_ != sq_ptr_) ::munmap(cq_ptr_, cq_sz_); sq_ptr_ = cq_ptr_ = nullptr; ::close(ring_fd_); ring_fd_ = -1; return false; }
    int ring_fd_ = -1; unsigned sq_entries_ = 0; std::size_t sq_sz_ = 0, cq_sz_ = 0, sqes_sz_ = 0; void* sq_ptr_ = nullptr; void* cq_ptr_ = nullptr;
    io_uring_sqe* sqes_ = nullptr; io_uring_cqe* cqes_ = nullptr;
    unsigned *sq_head_ = nullptr, *sq_tail_ = nullptr, *sq_mask_ = nullptr, *sq_array_ = nullptr, *cq_head_ = nullptr, *cq_tail_ = nullptr, *cq_mask_ = nullptr;
#endif
#if defined(FORGE_OS_WINDOWS)
    completion_port cp_; std::uint64_t next_key_ = 1; std::map<std::uint64_t, std::uint64_t> keys_;
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
    struct aio_op { struct aiocb cb; std::uint64_t user = 0; };
    std::vector<std::unique_ptr<aio_op>> aio_; int kq_ = -1;
#endif
    void threads_init_() {
        be_ = aio_backend::threads;
        for (int i = 0; i < 2; ++i) workers_.emplace_back([this] {
            for (;;) {
                tjob j;
                { std::unique_lock<std::mutex> g(tm_); tcv_.wait(g, [&] { return stop_ || !tq_.empty(); }); if (stop_ && tq_.empty()) return; j = tq_.front(); tq_.pop_front(); }
                file f(j.h); auto r = j.wr ? f.pwrite(j.buf, j.n, j.off) : f.pread(j.buf, j.n, j.off); (void)f.release();
                { std::lock_guard<std::mutex> g(tm_); tdone_.push_back({j.user, r ? std::int64_t(r.value) : -std::int64_t(r.err.code)}); }
                tcv_done_.notify_all();
            }
        });
    }
    struct tjob { file::native_t h{}; void* buf = nullptr; std::size_t n = 0; std::uint64_t off = 0; std::uint64_t user = 0; bool wr = false; };
    unsigned depth_; aio_backend be_ = aio_backend::threads; std::size_t inflight_ = 0; std::vector<io_completion> pending_;
    std::mutex tm_; std::condition_variable tcv_, tcv_done_; std::deque<tjob> tq_; std::vector<io_completion> tdone_; std::vector<std::thread> workers_; bool stop_ = false;
};
} // namespace forge::os
