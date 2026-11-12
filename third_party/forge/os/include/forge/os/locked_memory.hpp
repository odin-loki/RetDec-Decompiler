#pragma once
// Locked, non-dumpable memory for key material (FSS-CRY-2). forge::security::secure_buffer is this type; os code
// that cannot depend on forge::security (the keystore) uses it directly.
//
//   os::locked_buffer k(32);              // page-backed, zeroed
//   std::memcpy(k.data(), seed, 32);
//   if (!k.locked()) ...                  // best effort: mlock / VirtualLock may be refused (RLIMIT_MEMLOCK, quota)
//   // destruction, move-assignment over it or reset(): crypto_wipe, then unlock and unmap
//
// Each buffer is its own anonymous mapping (size rounded up to whole pages), so the bytes never share a page
// with ordinary heap data and the protections below apply to exactly this memory:
//   lock        POSIX mlock(2); Windows VirtualLock (the minimum working set is raised once if VirtualLock is
//               refused for quota). locked() reports the outcome; a failure is not an error.
//   no dump     Linux MADV_DONTDUMP (+ MADV_WIPEONFORK, so a forked child sees zeros), FreeBSD MADV_NOCORE.
//               Windows has no per-range core-dump exclusion: WerRegisterExcludedMemoryBlock keeps the range out of
//               Windows Error Reporting dumps (best effort; WER allows few blocks per process), but a debugger or
//               MiniDumpWriteDump still sees it. lockdown()'s job object (limits.core = 0 ->
//               DIE_ON_UNHANDLED_EXCEPTION) is the process-wide answer there. macOS has no equivalent either:
//               dump_excluded() is false; set limits.core = 0 in the policy.
//   guard pages optional (guard = true): one PROT_NONE / PAGE_NOACCESS page before and after, so a linear
//               overrun faults instead of reading or writing neighbouring memory.
//   wipe        crypto_wipe (Monocypher; not optimised away) over the whole mapping before it is released.
// Non-copyable; a move leaves the source empty. Allocation failure throws std::bad_alloc.
// Tests can observe the wipe: set_release_hook(fn) is called with the (wiped) bytes just before they are unmapped.
#include <forge/os/os.hpp>
#include <monocypher.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace forge::os {

class locked_buffer {
public:
    using release_hook = void (*)(const std::uint8_t* p, std::size_t n);

    locked_buffer() = default;
    explicit locked_buffer(std::size_t n, bool guard = false) { alloc_(n, guard); }
    locked_buffer(const void* src, std::size_t n, bool guard = false) { alloc_(n, guard); if (n) std::memcpy(p_, src, n); }
    locked_buffer(const locked_buffer&) = delete;
    locked_buffer& operator=(const locked_buffer&) = delete;
    locked_buffer(locked_buffer&& o) noexcept { steal_(o); }
    locked_buffer& operator=(locked_buffer&& o) noexcept { if (this != &o) { reset(); steal_(o); } return *this; }
    ~locked_buffer() { reset(); }

    std::uint8_t* data() noexcept { return p_; }
    const std::uint8_t* data() const noexcept { return p_; }
    std::size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    std::uint8_t* begin() noexcept { return p_; }
    std::uint8_t* end() noexcept { return p_ + n_; }
    const std::uint8_t* begin() const noexcept { return p_; }
    const std::uint8_t* end() const noexcept { return p_ + n_; }
    std::uint8_t& operator[](std::size_t i) noexcept { return p_[i]; }
    const std::uint8_t& operator[](std::size_t i) const noexcept { return p_[i]; }

    bool locked() const noexcept { return locked_; }          // mlock / VirtualLock succeeded
    bool dump_excluded() const noexcept { return no_dump_; }  // MADV_DONTDUMP / MADV_NOCORE / WER exclusion applied
    bool guarded() const noexcept { return guard_; }          // guard pages around the data

    // Wipe, unlock and release now (the buffer becomes empty).
    void reset() noexcept {
        if (!base_) return;
        crypto_wipe(body_(), body_size_());
        if (auto h = hook_().load()) h(p_, n_);
#if defined(FORGE_OS_WINDOWS)
        if (no_dump_) if (auto f = wer_fns_().unreg) f(body_());
        if (locked_) VirtualUnlock(body_(), body_size_());
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        if (locked_) ::munlock(body_(), body_size_());
        ::munmap(base_, map_);
#endif
        base_ = nullptr; p_ = nullptr; n_ = 0; map_ = 0; locked_ = no_dump_ = guard_ = padded_ = false;
    }

    static void set_release_hook(release_hook h) noexcept { hook_().store(h); }

private:
    static std::atomic<release_hook>& hook_() { static std::atomic<release_hook> h{nullptr}; return h; }
    std::uint8_t* body_() const noexcept { return static_cast<std::uint8_t*>(base_) + (padded_ ? page_size() : 0); }
    std::size_t body_size_() const noexcept { return map_ - (padded_ ? 2 * page_size() : 0); }
    void steal_(locked_buffer& o) noexcept {
        base_ = std::exchange(o.base_, nullptr); p_ = std::exchange(o.p_, nullptr); n_ = std::exchange(o.n_, 0); map_ = std::exchange(o.map_, 0);
        locked_ = std::exchange(o.locked_, false); no_dump_ = std::exchange(o.no_dump_, false); guard_ = std::exchange(o.guard_, false);
        padded_ = std::exchange(o.padded_, false);
    }
#if defined(FORGE_OS_WINDOWS)
    struct wer_fns { HRESULT(WINAPI* reg)(PVOID, DWORD) = nullptr; HRESULT(WINAPI* unreg)(PVOID) = nullptr; };
    static const wer_fns& wer_fns_() {
        static const wer_fns f = [] {
            wer_fns w; HMODULE k = GetModuleHandleW(L"kernel32.dll");
            if (k) {
                w.reg = reinterpret_cast<decltype(w.reg)>(reinterpret_cast<void*>(GetProcAddress(k, "WerRegisterExcludedMemoryBlock")));
                w.unreg = reinterpret_cast<decltype(w.unreg)>(reinterpret_cast<void*>(GetProcAddress(k, "WerUnregisterExcludedMemoryBlock")));
            }
            if (!w.reg || !w.unreg) w = {};
            return w;
        }();
        return f;
    }
#endif
    void alloc_(std::size_t n, bool guard) {
        const std::size_t pg = page_size();
        const std::size_t body = ((n ? n : 1) + pg - 1) / pg * pg;
        const std::size_t total = body + (guard ? 2 * pg : 0);
#if defined(FORGE_OS_WINDOWS)
        void* b = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!b) throw std::bad_alloc();
#else
        void* b = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (b == MAP_FAILED) throw std::bad_alloc();
#endif
        base_ = b; map_ = total; padded_ = guard; n_ = n;
        std::uint8_t* d = body_(); p_ = d;
        if (guard) {
#if defined(FORGE_OS_WINDOWS)
            DWORD old; guard_ = VirtualProtect(b, pg, PAGE_NOACCESS, &old) && VirtualProtect(d + body, pg, PAGE_NOACCESS, &old);
#else
            guard_ = ::mprotect(b, pg, PROT_NONE) == 0 && ::mprotect(d + body, pg, PROT_NONE) == 0;
#endif
        }
#if defined(__linux__)
        no_dump_ = ::madvise(d, body, MADV_DONTDUMP) == 0;
#if defined(MADV_WIPEONFORK)
        (void)::madvise(d, body, MADV_WIPEONFORK);
#endif
#elif defined(__FreeBSD__) && defined(MADV_NOCORE)
        no_dump_ = ::madvise(d, body, MADV_NOCORE) == 0;
#elif defined(FORGE_OS_WINDOWS)
        if (auto f = wer_fns_().reg) no_dump_ = SUCCEEDED(f(d, DWORD(body)));
#endif
#if defined(FORGE_OS_WINDOWS)
        locked_ = VirtualLock(d, body) != 0;
        if (!locked_ && GetLastError() == ERROR_WORKING_SET_QUOTA) {
            SIZE_T lo = 0, hi = 0;
            if (GetProcessWorkingSetSize(GetCurrentProcess(), &lo, &hi)) {
                SIZE_T want = lo + body + 16 * pg;
                if (SetProcessWorkingSetSize(GetCurrentProcess(), want, hi > want ? hi : want)) locked_ = VirtualLock(d, body) != 0;
            }
        }
#else
        locked_ = ::mlock(d, body) == 0;
#endif
    }

    void* base_ = nullptr;      // start of the mapping (guard page when guarded)
    std::uint8_t* p_ = nullptr; // the data
    std::size_t n_ = 0, map_ = 0;
    bool locked_ = false, no_dump_ = false, guard_ = false;
    bool padded_ = false;       // the mapping has a page before and after the data (guard_ says whether they are protected)
};

} // namespace forge::os
