// Generation-checked node handles (engine rule: handles, not owning pointers).
#pragma once

#include "fuse/ui/types.hpp"

namespace fuse::ui {

struct NodeHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
    explicit operator bool() const { return valid(); }
    friend bool operator==(const NodeHandle&, const NodeHandle&) = default;
};

struct ConnectionHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
    friend bool operator==(const ConnectionHandle&, const ConnectionHandle&) = default;
};

struct TimerHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
};

} // namespace fuse::ui
