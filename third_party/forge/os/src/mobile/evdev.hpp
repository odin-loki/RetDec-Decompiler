#pragma once
// evdev touchscreen reader shared by the Linux and FreeBSD backends (protocol B slots + single-touch BTN_TOUCH).
// Every direct-touch node (INPUT_PROP_DIRECT) is read by one thread. Per node:
//   - EVIOCSCLOCKID(CLOCK_MONOTONIC), so event times are on the monotonic_ns() clock (when the ioctl works);
//   - EVIOCGABS ranges map positions to display pixels when the display size is known ((v - min) / (max - min) *
//     size), else to device units counted from the axis minimum; pressure is normalised to 0..1 by its maximum;
//   - tracking ids of the second and later nodes are offset by node << 16 so two panels never share an id.
#include "internal.hpp"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <dev/evdev/input.h>
#ifndef input_event_sec
#define input_event_sec time.tv_sec
#define input_event_usec time.tv_usec
#endif
#else
#include <linux/input.h>
#endif
namespace forge::os::mobile::detail {

struct evdev_node { int fd = -1; double sx = 1, sy = 1, ox = 0, oy = 0, pmax = 0; };

// Direct touch (a touchscreen or tablet display), not a touchpad / pointer device.
inline bool evdev_is_direct(int fd) {
    unsigned long props[(INPUT_PROP_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))] = {};
    if (::ioctl(fd, EVIOCGPROP(sizeof props), props) < 0) return false;
    constexpr std::size_t bits = 8 * sizeof(unsigned long);
    return (props[INPUT_PROP_DIRECT / bits] >> (INPUT_PROP_DIRECT % bits)) & 1ul;
}
inline evdev_node evdev_setup(int fd, int width_px, int height_px) {
    evdev_node n; n.fd = fd;
    int clk = CLOCK_MONOTONIC; (void)::ioctl(fd, EVIOCSCLOCKID, &clk);
    auto axis = [&](int mt, int st, double size, double& scale, double& off) {
        input_absinfo a{};
        if (::ioctl(fd, EVIOCGABS(mt), &a) < 0 && ::ioctl(fd, EVIOCGABS(st), &a) < 0) return;
        off = a.minimum;
        if (size > 0 && a.maximum > a.minimum) scale = size / double(a.maximum - a.minimum);
    };
    axis(ABS_MT_POSITION_X, ABS_X, width_px, n.sx, n.ox); axis(ABS_MT_POSITION_Y, ABS_Y, height_px, n.sy, n.oy);
    input_absinfo p{};
    if ((::ioctl(fd, EVIOCGABS(ABS_MT_PRESSURE), &p) == 0 || ::ioctl(fd, EVIOCGABS(ABS_PRESSURE), &p) == 0) && p.maximum > 0) n.pmax = p.maximum;
    return n;
}

inline void evdev_loop(std::vector<evdev_node> nodes, queue& q, worker& w) {
    struct slot { int id = -1; double x = 0, y = 0, p = 1; bool changed = false, down = false, up = false; };
    struct state { std::vector<slot> slots = std::vector<slot>(16); int cur = 0; bool st_touch = false, st_changed = false, open = true; };
    std::vector<state> st(nodes.size());
    input_event buf[64];
    auto live = [&] { return std::any_of(st.begin(), st.end(), [](auto& s) { return s.open; }); };
    while (!w.stop && live()) {
        std::vector<pollfd> pf; for (auto& n : nodes) pf.push_back({n.fd, POLLIN, 0});
        int r = ::poll(pf.data(), nfds_t(pf.size()), 50);
        if (r < 0 && errno != EINTR) break; if (r <= 0) continue;
        for (std::size_t d = 0; d < nodes.size(); ++d) {
            if (!st[d].open || !(pf[d].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = ::read(nodes[d].fd, buf, sizeof buf);
            if (n == 0) { st[d].open = false; continue; }
            if (n < 0) { if (errno != EAGAIN && errno != EINTR) st[d].open = false; continue; }
            auto& nd = nodes[d]; auto& S = st[d]; auto& slots = S.slots;
            auto active = [&] { int c = 0; for (auto& s : slots) c += s.id >= 0; return c; };
            int id_base = int(d) << 16;
            for (std::size_t i = 0; i < std::size_t(n) / sizeof(input_event); ++i) {
                auto& ev = buf[i]; auto& s = slots[std::size_t(S.cur)];
                if (ev.type == EV_ABS) {
                    switch (ev.code) {
                    case ABS_MT_SLOT: S.cur = std::clamp(ev.value, 0, 15); break;
                    case ABS_MT_TRACKING_ID: if (ev.value >= 0) { s.id = id_base | (ev.value & 0xFFFF); s.down = true; } else { s.up = true; } s.changed = true; break;
                    case ABS_MT_POSITION_X: s.x = (ev.value - nd.ox) * nd.sx; s.changed = true; break;
                    case ABS_MT_POSITION_Y: s.y = (ev.value - nd.oy) * nd.sy; s.changed = true; break;
                    case ABS_MT_PRESSURE: s.p = nd.pmax > 0 ? ev.value / nd.pmax : ev.value; s.changed = true; break;
                    case ABS_X: if (s.id < 0 || S.cur == 0) { slots[0].x = (ev.value - nd.ox) * nd.sx; S.st_changed = true; } break;
                    case ABS_Y: if (s.id < 0 || S.cur == 0) { slots[0].y = (ev.value - nd.oy) * nd.sy; S.st_changed = true; } break;
                    }
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    S.st_touch = true; if (ev.value) { if (slots[0].id < 0) { slots[0].id = id_base; slots[0].down = true; } } else slots[0].up = true;
                    slots[0].changed = true;
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    std::uint64_t t = std::uint64_t(ev.input_event_sec) * 1000000000ull + std::uint64_t(ev.input_event_usec) * 1000ull;
                    if (S.st_touch && S.st_changed) slots[0].changed = true;
                    for (auto& sl : slots) {
                        if (!sl.changed) continue;
                        touch_phase ph = sl.up ? touch_phase::up : sl.down ? touch_phase::down : touch_phase::move;
                        q.push({true, {}, {t, ph, sl.id, sl.x, sl.y, sl.p, active()}});
                        if (sl.up) sl.id = -1;
                        sl.changed = sl.down = sl.up = false;
                    }
                    S.st_changed = false;
                }
            }
        }
    }
    for (auto& n : nodes) ::close(n.fd);
    q.finish();
}

} // namespace forge::os::mobile::detail
