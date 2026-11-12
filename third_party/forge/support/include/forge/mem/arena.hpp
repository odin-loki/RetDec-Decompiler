#pragma once
// Bump arenas: linear (reset all at once) and a scope marker for stack-like use. Alignment is a power of two.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace forge::mem {
class arena {
public:
    explicit arena(std::size_t block = 1 << 20) : block_(block) {}
    void* alloc(std::size_t n, std::size_t align = alignof(std::max_align_t)) {
        if (align & (align - 1)) throw std::bad_alloc();
        for (;;) {
            if (cur_ < blocks_.size()) {
                auto& b = blocks_[cur_];
                std::uintptr_t base = reinterpret_cast<std::uintptr_t>(b.p.get()), p = (base + off_ + align - 1) & ~(align - 1);
                if (p + n <= base + b.n) { off_ = p + n - base; used_ += n; return reinterpret_cast<void*>(p); }
                if (cur_ + 1 < blocks_.size()) { ++cur_; off_ = 0; continue; }
            }
            std::size_t sz = std::max(block_, n + align);
            blocks_.push_back({std::unique_ptr<std::byte[]>(new std::byte[sz]), sz});
            cur_ = blocks_.size() - 1; off_ = 0;
        }
    }
    template <class T> T* make_array(std::size_t n) { auto* p = static_cast<T*>(alloc(n * sizeof(T), alignof(T))); for (std::size_t i = 0; i < n; ++i) new (p + i) T(); return p; }
    struct mark { std::size_t cur, off, used; };
    mark save() const { return {cur_, off_, used_}; }
    void restore(mark m) { cur_ = m.cur; off_ = m.off; used_ = m.used; }
    void reset() { cur_ = 0; off_ = 0; used_ = 0; }
    std::size_t used() const { return used_; }
    std::size_t reserved() const { std::size_t s = 0; for (auto& b : blocks_) s += b.n; return s; }
private:
    struct blk { std::unique_ptr<std::byte[]> p; std::size_t n; };
    std::vector<blk> blocks_;
    std::size_t block_, cur_ = 0, off_ = 0, used_ = 0;
};
struct scope { arena& a; arena::mark m; explicit scope(arena& x) : a(x), m(x.save()) {} ~scope() { a.restore(m); } };
}
