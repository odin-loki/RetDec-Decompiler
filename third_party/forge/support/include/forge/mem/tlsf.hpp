#pragma once
// Allocators for frame/game/inference workloads: a fixed-size block pool and a TLSF (two-level segregated
// fit) general allocator over a caller-provided region — O(1) malloc/free, immediate coalescing, allocation
// tracking (live bytes, peak, count) and a leak report.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace forge::mem {

class pool {   // fixed-size blocks, intrusive free list
public:
    pool(std::size_t block, std::size_t count) : bs_((std::max(block, sizeof(void*)) + 15) & ~std::size_t(15)), buf_(bs_ * count + 16) {
        auto base = (reinterpret_cast<std::uintptr_t>(buf_.data()) + 15) & ~std::uintptr_t(15);
        for (std::size_t i = count; i-- > 0;) { void* p = reinterpret_cast<void*>(base + i * bs_); *static_cast<void**>(p) = head_; head_ = p; }
    }
    void* alloc() { if (!head_) return nullptr; void* p = head_; head_ = *static_cast<void**>(p); ++live_; return p; }
    void free(void* p) { if (!p) return; *static_cast<void**>(p) = head_; head_ = p; --live_; }
    std::size_t live() const { return live_; }
    std::size_t block_size() const { return bs_; }
private:
    std::size_t bs_; std::vector<std::uint8_t> buf_; void* head_ = nullptr; std::size_t live_ = 0;
};

class tlsf {
    static constexpr std::size_t align = 16, sl_log = 4, sl_n = 1 << sl_log, small = 256, fl_n = 40;
    struct alignas(16) hdr { std::size_t size; hdr* prev_phys; hdr* nf; hdr* pf; std::size_t free; };
public:
    struct stats { std::size_t live_bytes = 0, peak_bytes = 0, live_allocs = 0, free_bytes = 0, largest_free = 0; };

    tlsf(void* mem, std::size_t bytes) {
        auto b = (reinterpret_cast<std::uintptr_t>(mem) + align - 1) & ~std::uintptr_t(align - 1);
        auto e = (reinterpret_cast<std::uintptr_t>(mem) + bytes) & ~std::uintptr_t(align - 1);
        if (e <= b + sizeof(hdr) + align) throw std::invalid_argument("tlsf: region too small");
        begin_ = reinterpret_cast<std::uint8_t*>(b); end_ = reinterpret_cast<std::uint8_t*>(e);
        auto* h = reinterpret_cast<hdr*>(begin_); h->size = std::size_t(e - b) - sizeof(hdr); h->prev_phys = nullptr; h->free = 1;
        insert(h);
    }
    void* malloc(std::size_t n) {
        n = n ? (n + align - 1) & ~(align - 1) : align;
        std::size_t want = n >= small ? n + (std::size_t(1) << (log2(n) - sl_log)) - 1 : n;   // round up to the next class
        std::size_t fl, sl; map(want, fl, sl);
        hdr* h = find(fl, sl);
        if (!h) { map(n, fl, sl); for (h = lists_[fl][sl]; h && h->size < n; h = h->nf) {} }   // exact class, first fit
        if (!h) return nullptr;
        remove(h);
        if (h->size >= n + sizeof(hdr) + align) {          // split off the remainder
            auto* r = reinterpret_cast<hdr*>(reinterpret_cast<std::uint8_t*>(h + 1) + n);
            r->size = h->size - n - sizeof(hdr); r->prev_phys = h; r->free = 1;
            if (auto* nx = next(r)) nx->prev_phys = r;
            h->size = n; insert(r);
        }
        h->free = 0; st_.live_bytes += h->size; ++st_.live_allocs; st_.peak_bytes = std::max(st_.peak_bytes, st_.live_bytes);
        return h + 1;
    }
    void free(void* p) {
        if (!p) return;
        auto* h = static_cast<hdr*>(p) - 1;
        if (reinterpret_cast<std::uint8_t*>(h) < begin_ || reinterpret_cast<std::uint8_t*>(h) >= end_ || h->free) throw std::invalid_argument("tlsf: bad or double free");
        st_.live_bytes -= h->size; --st_.live_allocs; h->free = 1;
        if (auto* nx = next(h); nx && nx->free) { remove(nx); h->size += sizeof(hdr) + nx->size; if (auto* nn = next(h)) nn->prev_phys = h; }
        if (auto* pv = h->prev_phys; pv && pv->free) { remove(pv); pv->size += sizeof(hdr) + h->size; if (auto* nn = next(pv)) nn->prev_phys = pv; h = pv; }
        insert(h);
    }
    stats stat() const {
        stats s = st_;
        for (auto* h = reinterpret_cast<const hdr*>(begin_); h; h = next(const_cast<hdr*>(h)))
            if (h->free) { s.free_bytes += h->size; s.largest_free = std::max(s.largest_free, h->size); }
        return s;
    }
    // One line per live allocation (offset and size); empty when nothing leaked.
    std::vector<std::string> leak_report() const {
        std::vector<std::string> r;
        for (auto* h = reinterpret_cast<const hdr*>(begin_); h; h = next(const_cast<hdr*>(h)))
            if (!h->free) r.push_back("leak @" + std::to_string(reinterpret_cast<const std::uint8_t*>(h + 1) - begin_) + " " + std::to_string(h->size) + " bytes");
        return r;
    }
    // Walks the physical chain and the free lists; throws on any inconsistency.
    void check() const {
        const hdr* prev = nullptr; std::size_t nfree = 0;
        for (auto* h = reinterpret_cast<const hdr*>(begin_); h; prev = h, h = next(const_cast<hdr*>(h))) {
            if (h->prev_phys != prev) throw std::logic_error("tlsf: prev_phys broken");
            if (h->free && prev && prev->free) throw std::logic_error("tlsf: adjacent free blocks not merged");
            nfree += h->free;
        }
        std::size_t listed = 0;
        for (std::size_t f = 0; f < fl_n; ++f) for (std::size_t s = 0; s < sl_n; ++s) for (auto* h = lists_[f][s]; h; h = h->nf) {
            std::size_t a, b; map(h->size, a, b); if (a != f || b != s || !h->free) throw std::logic_error("tlsf: block in wrong list"); ++listed;
            if (!((fl_bm_ >> f) & 1) || !((sl_bm_[f] >> s) & 1)) throw std::logic_error("tlsf: bitmap out of sync");
        }
        if (listed != nfree) throw std::logic_error("tlsf: free list count mismatch");
    }
private:
    static std::size_t log2(std::size_t v) { return std::size_t(std::bit_width(v) - 1); }
    static void map(std::size_t n, std::size_t& fl, std::size_t& sl) {
        if (n < small) { fl = 0; sl = n / (small / sl_n); return; }
        std::size_t l = log2(n); fl = l - log2(small) + 1; sl = (n >> (l - sl_log)) & (sl_n - 1);
        if (fl >= fl_n) { fl = fl_n - 1; sl = sl_n - 1; }
    }
    hdr* next(hdr* h) const { auto* p = reinterpret_cast<std::uint8_t*>(h + 1) + h->size; return p + sizeof(hdr) <= end_ ? reinterpret_cast<hdr*>(p) : nullptr; }
    void insert(hdr* h) {
        std::size_t f, s; map(h->size, f, s);
        h->pf = nullptr; h->nf = lists_[f][s]; if (h->nf) h->nf->pf = h; lists_[f][s] = h;
        fl_bm_ |= std::uint64_t(1) << f; sl_bm_[f] |= 1u << s;
    }
    void remove(hdr* h) {
        std::size_t f, s; map(h->size, f, s);
        if (h->pf) h->pf->nf = h->nf; else lists_[f][s] = h->nf;
        if (h->nf) h->nf->pf = h->pf;
        if (!lists_[f][s]) { sl_bm_[f] &= ~(1u << s); if (!sl_bm_[f]) fl_bm_ &= ~(std::uint64_t(1) << f); }
    }
    hdr* find(std::size_t fl, std::size_t sl) const {
        if (fl >= fl_n) return nullptr;
        std::uint32_t m = sl_bm_[fl] & (~0u << sl);
        if (!m) { std::uint64_t fm = fl + 1 < 64 ? fl_bm_ & (~std::uint64_t(0) << (fl + 1)) : 0; if (!fm) return nullptr;
                  fl = std::size_t(std::countr_zero(fm)); m = sl_bm_[fl]; }
        return lists_[fl][std::size_t(std::countr_zero(m))];
    }
    std::uint8_t *begin_, *end_;
    hdr* lists_[fl_n][sl_n] = {};
    std::uint64_t fl_bm_ = 0; std::uint32_t sl_bm_[fl_n] = {};
    stats st_;
};

} // namespace forge::mem
