// Small-buffer callable for signals, timers and commands: no heap, trivially copyable captures only.
#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace fuse::ui {

template <class Sig, std::size_t Cap = 32>
class InlineFn;

template <class R, class... Args, std::size_t Cap>
class InlineFn<R(Args...), Cap> {
public:
    InlineFn() = default;
    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, InlineFn> && std::is_invocable_r_v<R, F&, Args...>)
    InlineFn(F f) { // NOLINT(google-explicit-constructor)
        using Fn = std::decay_t<F>;
        static_assert(sizeof(Fn) <= Cap, "InlineFn: capture too large (keep captures to a few pointers / handles)");
        static_assert(std::is_trivially_copyable_v<Fn>, "InlineFn: captures must be trivially copyable");
        static_assert(alignof(Fn) <= alignof(std::max_align_t));
        ::new (static_cast<void*>(m_buf)) Fn(std::move(f));
        m_call = [](void* b, Args... a) -> R { return (*std::launder(static_cast<Fn*>(b)))(std::forward<Args>(a)...); };
    }
    explicit operator bool() const { return m_call != nullptr; }
    R operator()(Args... a) const { return m_call(const_cast<unsigned char*>(m_buf), std::forward<Args>(a)...); }

private:
    alignas(std::max_align_t) unsigned char m_buf[Cap]{};
    R (*m_call)(void*, Args...) = nullptr;
};

/// Generic typed signal for non-node use (models, app services). Fixed slot capacity, no allocation.
template <std::size_t MaxSlots, class... Args>
class Signal {
public:
    using Slot = InlineFn<void(Args...)>;
    /// Returns a slot id (0 on overflow).
    unsigned connect(Slot s) {
        for (unsigned i = 0; i < MaxSlots; ++i)
            if (!m_slots[i]) {
                m_slots[i] = s;
                return i + 1;
            }
        return 0;
    }
    void disconnect(unsigned id) {
        if (id && id <= MaxSlots) m_slots[id - 1] = Slot{};
    }
    void emit(Args... a) const {
        for (const auto& s : m_slots)
            if (s) s(a...);
    }

private:
    Slot m_slots[MaxSlots]{};
};

} // namespace fuse::ui
