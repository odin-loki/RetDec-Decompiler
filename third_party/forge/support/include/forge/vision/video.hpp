#pragma once
// Hardware video decode hooks (Part F.1 `vision`): one interface over NVDEC, VA-API, VideoToolbox and MediaCodec.
// Frames come back as NV12 in host memory (device-resident output is a later step; see gpu interop).
//
// Status (see docs/STATUS.md): HARDWARE-UNTESTED.
//   nvdec        full H.264/HEVC/AV1/VP9 decode via cuvid's own parser; compiled only when nvcuvid.h is found
//                (NVIDIA Video Codec SDK headers, -DFORGE_NVCUVID_INCLUDE=...); libcuda/libnvcuvid are dlopened.
//   videotoolbox full H.264/HEVC decode (gpu/src/video_vt.mm, Apple only).
//   mediacodec   full decode through the NDK AMediaCodec API (Android only, links mediandk).
//   vaapi        device + profile probe through a dlopened libva; bitstream decode needs a slice-header parser
//                and is NOT implemented (decode() reports unsupported).
// The Annex-B splitter and parameter-set extraction are pure CPU code and are unit-tested.
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <forge/core/dl.hpp>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace forge::vision::video {
enum class codec { h264, hevc, vp9, av1 };
inline const char* codec_name(codec c) { switch (c) { case codec::h264: return "h264"; case codec::hevc: return "hevc"; case codec::vp9: return "vp9"; case codec::av1: return "av1"; } return "?"; }

struct frame {
    int width = 0, height = 0;
    std::int64_t pts = 0;
    std::vector<std::uint8_t> y, uv;   // NV12: y [h][w], uv [h/2][w] interleaved
};
using frame_cb = std::function<void(frame&&)>;

class decoder {
public:
    virtual ~decoder() = default;
    virtual const char* backend() const = 0;
    // Feed one access unit / chunk of an Annex-B (H.264/HEVC) or OBU/IVF-frame (AV1/VP9) elementary stream.
    virtual bool decode(std::span<const std::uint8_t> data, std::int64_t pts, std::string* why) = 0;
    virtual void flush() = 0;
};

// ---- Annex-B helpers ----
struct nal { std::span<const std::uint8_t> bytes; int type = 0; };   // bytes exclude the start code
inline std::vector<nal> split_annexb(std::span<const std::uint8_t> s, codec c = codec::h264) {
    std::vector<nal> out; std::size_t i = 0, n = s.size(), start = SIZE_MAX;
    auto push = [&](std::size_t b, std::size_t e) {
        while (e > b && s[e - 1] == 0) --e;   // trailing zero bytes belong to the next start code
        if (e <= b) return;
        nal x; x.bytes = s.subspan(b, e - b);
        x.type = c == codec::hevc ? (s[b] >> 1) & 0x3f : s[b] & 0x1f;
        out.push_back(x);
    };
    while (i + 2 < n) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            if (start != SIZE_MAX) push(start, i);
            i += 3; start = i; continue;
        }
        ++i;
    }
    if (start != SIZE_MAX) push(start, n);
    return out;
}
// AVCC/HVCC (4-byte length prefixed) -> Annex-B.
inline std::vector<std::uint8_t> length_prefixed_to_annexb(std::span<const std::uint8_t> s) {
    std::vector<std::uint8_t> o; std::size_t i = 0;
    while (i + 4 <= s.size()) {
        std::size_t l = (std::size_t(s[i]) << 24) | (std::size_t(s[i+1]) << 16) | (std::size_t(s[i+2]) << 8) | s[i+3]; i += 4;
        if (l > s.size() - i) break;
        static const std::uint8_t sc[4] = {0, 0, 0, 1}; o.insert(o.end(), sc, sc + 4); o.insert(o.end(), s.begin() + i, s.begin() + i + l); i += l;
    }
    return o;
}
// H.264 SPS: width/height from the exp-Golomb fields (needed by VideoToolbox/MediaCodec format setup).
struct bitreader {
    std::vector<std::uint8_t> b; std::size_t pos = 0;
    explicit bitreader(std::span<const std::uint8_t> s) { for (std::size_t i = 0; i < s.size(); ++i) { if (i >= 2 && s[i] == 3 && s[i-1] == 0 && s[i-2] == 0) continue; b.push_back(s[i]); } }   // drop emulation bytes
    unsigned u(int n) { unsigned v = 0; while (n--) { v = (v << 1) | (pos / 8 < b.size() ? (b[pos / 8] >> (7 - pos % 8)) & 1 : 0); ++pos; } return v; }
    unsigned ue() { int z = 0; while (u(1) == 0 && z < 32) ++z; return (1u << z) - 1 + u(z); }
    int se() { unsigned k = ue(); return k & 1 ? int((k + 1) / 2) : -int(k / 2); }
};
inline bool h264_sps_size(std::span<const std::uint8_t> sps, int& w, int& h) {
    if (sps.size() < 4 || (sps[0] & 0x1f) != 7) return false;
    bitreader r(sps.subspan(1));
    unsigned profile = r.u(8); r.u(16); r.ue();
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        chroma = r.ue(); if (chroma == 3) r.u(1); r.ue(); r.ue(); r.u(1);
        if (r.u(1)) for (int i = 0; i < (chroma != 3 ? 8 : 12); ++i) if (r.u(1)) { int last = 8, next = 8; for (int j = 0; j < (i < 6 ? 16 : 64); ++j) { if (next) next = (last + r.se() + 256) % 256; last = next ? next : last; } }
    }
    r.ue(); unsigned poc = r.ue();
    if (poc == 0) r.ue(); else if (poc == 1) { r.u(1); r.se(); r.se(); unsigned c = r.ue(); for (unsigned i = 0; i < c && i < 256; ++i) r.se(); }
    r.ue(); r.u(1);
    unsigned wm = r.ue(), hm = r.ue(), frame_mbs_only = r.u(1);
    if (!frame_mbs_only) r.u(1);
    r.u(1);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (r.u(1)) { cl = r.ue(); cr = r.ue(); ct = r.ue(); cb = r.ue(); }
    unsigned sx = chroma == 0 || chroma == 3 ? 1 : 2, sy = (chroma == 1 ? 2 : 1) * (2 - frame_mbs_only);
    w = int((wm + 1) * 16 - sx * (cl + cr)); h = int((2 - frame_mbs_only) * (hm + 1) * 16 - sy * (ct + cb));
    return w > 0 && h > 0;
}

// ---- VA-API probe (dlopen libva) ----
struct vaapi_info { bool ok = false; std::string driver, why; std::vector<codec> decodable; };
inline vaapi_info vaapi_probe(const char* node = "/dev/dri/renderD128") {
    vaapi_info r;
#if defined(__linux__) && !defined(__ANDROID__)
    void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL); void* vd = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!va || !vd) { r.why = "libva not found"; return r; }
    using get_t = void* (*)(int); using init_t = int (*)(void*, int*, int*); using term_t = int (*)(void*);
    using maxp_t = int (*)(void*); using qp_t = int (*)(void*, int*, int*); using maxe_t = int (*)(void*);
    using qe_t = int (*)(void*, int, int*, int*); using vend_t = const char* (*)(void*);
    auto get = (get_t)dlsym(vd, "vaGetDisplayDRM"); auto init = (init_t)dlsym(va, "vaInitialize"); auto term = (term_t)dlsym(va, "vaTerminate");
    auto maxp = (maxp_t)dlsym(va, "vaMaxNumProfiles"); auto qp = (qp_t)dlsym(va, "vaQueryConfigProfiles");
    auto maxe = (maxe_t)dlsym(va, "vaMaxNumEntrypoints"); auto qe = (qe_t)dlsym(va, "vaQueryConfigEntrypoints"); auto vend = (vend_t)dlsym(va, "vaQueryVendorString");
    if (!get || !init || !term || !maxp || !qp || !maxe || !qe) { r.why = "libva symbols missing"; return r; }
    int fd = ::open(node, O_RDWR);
    if (fd < 0) { r.why = std::string("no DRM render node ") + node; return r; }
    void* dpy = get(fd); int maj = 0, min = 0;
    if (!dpy || init(dpy, &maj, &min) != 0) { ::close(fd); r.why = "vaInitialize failed"; return r; }
    if (vend) if (auto* v = vend(dpy)) r.driver = v;
    std::vector<int> prof(std::size_t(std::max(maxp(dpy), 1))); int np = 0; qp(dpy, prof.data(), &np);
    std::vector<int> ep(std::size_t(std::max(maxe(dpy), 1)));
    auto vld = [&](int p) { int ne = 0; if (qe(dpy, p, ep.data(), &ne) != 0) return false; for (int i = 0; i < ne; ++i) if (ep[std::size_t(i)] == 1 /*VAEntrypointVLD*/) return true; return false; };
    // VAProfile values (va.h, stable ABI): H264High 7, HEVCMain 17, VP9Profile0 19, AV1Profile0 32
    std::pair<int, codec> want[] = {{7, codec::h264}, {17, codec::hevc}, {19, codec::vp9}, {32, codec::av1}};
    for (auto [p, c] : want) for (int i = 0; i < np; ++i) if (prof[std::size_t(i)] == p && vld(p)) { r.decodable.push_back(c); break; }
    term(dpy); ::close(fd); r.ok = true;
#else
    (void)node; r.why = "VA-API is Linux-only";
#endif
    return r;
}

// Opens the best decoder for `c` ("auto", "nvdec", "videotoolbox", "mediacodec", "vaapi"); null + why when none.
std::unique_ptr<decoder> open_decoder(codec c, frame_cb cb, const std::string& backend = "auto", std::string* why = nullptr);
} // namespace forge::vision::video
