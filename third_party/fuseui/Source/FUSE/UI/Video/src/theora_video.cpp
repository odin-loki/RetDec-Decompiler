#include "fuse/ui/video/theora_video.hpp"

#include <theora/theoradec.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fuse::ui::video {

struct TheoraVideo::Impl {
    std::vector<u8> bytes; ///< packet payloads, back to back
    struct Pkt { u32 offset = 0, size = 0; bool key = false; };
    std::vector<Pkt> headers, frames;
    th_info info{};
    th_comment comment{};
    th_setup_info* setup = nullptr;
    th_dec_ctx* dec = nullptr;
    u32 next = 0;          ///< next frame packet to feed
    i64 last = -1;         ///< last frame fed (its picture is the decoder's current output)
    u64 fed = 0;
    widgets::VideoSource src{};
    bool ok = false;

    Impl() { th_info_init(&info); th_comment_init(&comment); }
    ~Impl() { reset(); }
    void reset() {
        if (dec) th_decode_free(dec);
        dec = nullptr;
        if (setup) th_setup_free(setup);
        setup = nullptr;
        th_comment_clear(&comment);
        th_info_clear(&info);
        th_info_init(&info);
        th_comment_init(&comment);
        bytes.clear(); headers.clear(); frames.clear();
        next = 0; last = -1; fed = 0; ok = false;
    }
    ogg_packet packet(const Pkt& p, i64 no, bool bos) {
        ogg_packet op{};
        op.packet = p.size ? bytes.data() + p.offset : bytes.data();
        op.bytes = long(p.size);
        op.b_o_s = bos ? 1 : 0;
        op.e_o_s = 0;
        op.granulepos = -1;
        op.packetno = no;
        return op;
    }
    bool restartDecoder() {
        if (dec) th_decode_free(dec);
        dec = th_decode_alloc(&info, setup);
        next = 0;
        last = -1;
        return dec != nullptr;
    }
    void toRgba(u32* out) {
        th_ycbcr_buffer yuv;
        if (th_decode_ycbcr_out(dec, yuv) != 0) return;
        const u32 px = info.pic_x, py = info.pic_y, w = info.pic_width, h = info.pic_height;
        const int xs = info.pixel_fmt == TH_PF_444 ? 0 : 1, ys = info.pixel_fmt == TH_PF_420 ? 1 : 0;
        auto clamp8 = [](int v) { return u32(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (u32 y = 0; y < h; ++y) {
            const u32 sy = py + y;
            const u8* yr = yuv[0].data + std::ptrdiff_t(sy) * yuv[0].stride;
            const u8* ur = yuv[1].data + std::ptrdiff_t(sy >> ys) * yuv[1].stride;
            const u8* vr = yuv[2].data + std::ptrdiff_t(sy >> ys) * yuv[2].stride;
            u32* o = out + std::size_t(y) * w;
            for (u32 x = 0; x < w; ++x) {
                const u32 sx = px + x;
                // BT.601 video range, 16.16 fixed point
                const int c = (int(yr[sx]) - 16) * 76309, d = int(ur[sx >> xs]) - 128, e = int(vr[sx >> xs]) - 128;
                const u32 r = clamp8((c + 104597 * e + 32768) >> 16);
                const u32 g = clamp8((c - 25675 * d - 53279 * e + 32768) >> 16);
                const u32 b = clamp8((c + 132201 * d + 32768) >> 16);
                o[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }
    }
};

TheoraVideo::TheoraVideo() : m(std::make_unique<Impl>()) {}
TheoraVideo::~TheoraVideo() = default;

bool TheoraVideo::open(std::vector<u8> file, std::string* error) {
    m->reset();
    auto fail = [&](const char* msg) { if (error) *error = msg; m->reset(); return false; };
    ogg_sync_state sync;
    ogg_sync_init(&sync);
    char* buf = ogg_sync_buffer(&sync, long(file.size()));
    if (!file.empty()) std::memcpy(buf, file.data(), file.size());
    ogg_sync_wrote(&sync, long(file.size()));
    struct Stream { int serial; ogg_stream_state os; bool theora = false, rejected = false; };
    std::vector<Stream> streams;
    int theoraSerial = -1;
    th_setup_info* setup = nullptr;
    ogg_page page;
    bool bad = false;
    while (!bad && ogg_sync_pageout(&sync, &page) == 1) {
        const int serial = ogg_page_serialno(&page);
        auto it = std::find_if(streams.begin(), streams.end(), [&](const Stream& s) { return s.serial == serial; });
        if (it == streams.end()) {
            if (!ogg_page_bos(&page)) continue;
            streams.push_back({serial, {}});
            ogg_stream_init(&streams.back().os, serial);
            it = streams.end() - 1;
        }
        if (it->rejected || (theoraSerial >= 0 && serial != theoraSerial)) continue;
        ogg_stream_pagein(&it->os, &page);
        ogg_packet op;
        while (ogg_stream_packetout(&it->os, &op) == 1) {
            if (!it->theora && theoraSerial < 0) {
                // the first packet of a stream decides whether it is Theora
                if (th_decode_headerin(&m->info, &m->comment, &setup, &op) > 0) { it->theora = true; theoraSerial = serial; }
                else { it->rejected = true; break; }
                Impl::Pkt p{u32(m->bytes.size()), u32(op.bytes), false};
                m->bytes.insert(m->bytes.end(), op.packet, op.packet + op.bytes);
                m->headers.push_back(p);
                continue;
            }
            if (m->headers.size() < 3) {
                const int r = th_decode_headerin(&m->info, &m->comment, &setup, &op);
                if (r < 0) { bad = true; break; }
                Impl::Pkt p{u32(m->bytes.size()), u32(op.bytes), false};
                m->bytes.insert(m->bytes.end(), op.packet, op.packet + op.bytes);
                m->headers.push_back(p);
                if (r > 0) continue;
                // r == 0: that was the first data packet (headers complete): index it as frame 0
                m->headers.pop_back();
            }
            Impl::Pkt p{u32(m->bytes.size()), u32(op.bytes), th_packet_iskeyframe(&op) == 1};
            m->bytes.insert(m->bytes.end(), op.packet, op.packet + op.bytes);
            m->frames.push_back(p);
        }
    }
    for (Stream& s : streams) ogg_stream_clear(&s.os);
    ogg_sync_clear(&sync);
    m->setup = setup;
    if (bad) return fail("corrupt Theora header");
    if (theoraSerial < 0) return fail("no Theora stream");
    if (!setup || m->headers.size() < 3) return fail("incomplete Theora headers");
    if (m->frames.empty()) return fail("no video frames");
    if (!m->frames.front().key) m->frames.front().key = true; // a stream must start decodable
    if (!m->restartDecoder()) return fail("th_decode_alloc failed");
    m->ok = true;
    m->src.user = m.get();
    m->src.width = m->info.pic_width;
    m->src.height = m->info.pic_height;
    m->src.frames = u32(m->frames.size());
    m->src.fps = m->info.fps_denominator ? f32(double(m->info.fps_numerator) / m->info.fps_denominator) : 30.f;
    m->src.frame = [](void* user, u32 index, u32* rgba) {
        Impl* im = static_cast<Impl*>(user);
        if (!im->ok || index >= im->frames.size()) return;
        // the same decode path as TheoraVideo::decode (Impl-only so the source needs no back pointer)
        const u32 i = index;
        if (i64(i) != im->last) {
            u32 k = i;
            while (k > 0 && !im->frames[k].key) --k;
            if (im->last < 0 || i64(i) < im->last || i64(k) > im->last) {
                if (!im->restartDecoder()) return;
                im->next = k; // a fresh decoder starts at a keyframe
            }
            for (; im->next <= i; ++im->next) {
                ogg_packet op = im->packet(im->frames[im->next], i64(im->headers.size() + im->next), false);
                const int r = th_decode_packetin(im->dec, &op, nullptr);
                ++im->fed;
                if (r < 0 && r != TH_DUPFRAME) { im->last = -1; return; }
            }
            im->last = i;
        }
        im->toRgba(rgba);
    };
    return true;
}

bool TheoraVideo::openFile(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot read " + path; return false; }
    std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return open(std::move(data), error);
}

bool TheoraVideo::valid() const { return m->ok; }
u32 TheoraVideo::width() const { return m->ok ? m->info.pic_width : 0u; }
u32 TheoraVideo::height() const { return m->ok ? m->info.pic_height : 0u; }
u32 TheoraVideo::frameCount() const { return u32(m->frames.size()); }
f32 TheoraVideo::fps() const { return m->src.fps; }
bool TheoraVideo::isKeyframe(u32 i) const { return i < m->frames.size() && m->frames[i].key; }
u64 TheoraVideo::packetsDecoded() const { return m->fed; }
const widgets::VideoSource& TheoraVideo::source() const { return m->src; }

bool TheoraVideo::decode(u32 index, u32* rgba) {
    if (!m->ok || index >= m->frames.size()) return false;
    m->src.frame(m.get(), index, rgba);
    return m->last == i64(index);
}

std::string theoraVersion() { return th_version_string(); }

} // namespace fuse::ui::video
