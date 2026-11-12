// fuse_ui_theora_video: Theora decode behind the video widget (vendored libtheora 1.2.0).
//   - encodes a 48-frame 160x96 clip (moving square over a gradient, keyframe every 12) with the vendored encoder into
//     an in-memory Ogg file, then decodes it through TheoraVideo: frame count, size, fps, keyframe index, PSNR vs the
//     source above 30 dB on every frame
//   - random access: a seek decodes from the nearest keyframe (packet count probe) and is bit-identical to sequential
//     decode; the next frame costs one packet
//   - rejects garbage and audio-only input
//   - the video widget plays it (frames advance with time, scrubber seek)
// Optional --out=<dir>: theora_video.png and the clip as fuse_ui_test.ogv.
#include "test_util.hpp"

#include "fuse/ui/video/theora_video.hpp"

#include <theora/theoraenc.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
constexpr int W = 160, H = 96, N = 48;

void sourceRgb(int f, int x, int y, int& r, int& g, int& b) {
    r = (x * 255) / (W - 1);
    g = (y * 255) / (H - 1);
    b = 96;
    const int sx = 10 + f * 2, sy = 20 + (f % 24);
    if (x >= sx && x < sx + 32 && y >= sy && y < sy + 32) { r = 240; g = 240; b = 32; }
}

std::vector<u8> encodeClip() {
    th_info ti;
    th_info_init(&ti);
    ti.frame_width = W; ti.frame_height = H; ti.pic_width = W; ti.pic_height = H; ti.pic_x = 0; ti.pic_y = 0;
    ti.colorspace = TH_CS_ITU_REC_470BG; ti.pixel_fmt = TH_PF_420;
    ti.fps_numerator = 24; ti.fps_denominator = 1; ti.aspect_numerator = 1; ti.aspect_denominator = 1;
    ti.target_bitrate = 0; ti.quality = 56;
    ti.keyframe_granule_shift = 6;
    th_enc_ctx* enc = th_encode_alloc(&ti);
    th_info_clear(&ti);
    if (!enc) return {};
    int kf = 12;
    th_encode_ctl(enc, TH_ENCCTL_SET_KEYFRAME_FREQUENCY_FORCE, &kf, sizeof kf);
    std::vector<u8> out;
    ogg_stream_state os;
    ogg_stream_init(&os, 0x5eed);
    auto drain = [&](bool flush) {
        ogg_page pg;
        while (flush ? ogg_stream_flush(&os, &pg) : ogg_stream_pageout(&os, &pg)) {
            out.insert(out.end(), pg.header, pg.header + pg.header_len);
            out.insert(out.end(), pg.body, pg.body + pg.body_len);
        }
    };
    th_comment tc;
    th_comment_init(&tc);
    ogg_packet op;
    bool first = true;
    while (th_encode_flushheader(enc, &tc, &op) > 0) {
        ogg_stream_packetin(&os, &op);
        if (first) { drain(true); first = false; }
    }
    drain(true);
    th_comment_clear(&tc);
    std::vector<u8> Y(W * H), U(W * H / 4), V(W * H / 4);
    for (int f = 0; f < N; ++f) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int r, g, b;
                sourceRgb(f, x, y, r, g, b);
                Y[y * W + x] = u8(std::lround(16 + (65.738 * r + 129.057 * g + 25.064 * b) / 256));
            }
        for (int y = 0; y < H / 2; ++y)
            for (int x = 0; x < W / 2; ++x) {
                double cb = 0, cr = 0;
                for (int k = 0; k < 4; ++k) {
                    int r, g, b;
                    sourceRgb(f, x * 2 + (k & 1), y * 2 + (k >> 1), r, g, b);
                    cb += 128 + (-37.945 * r - 74.494 * g + 112.439 * b) / 256;
                    cr += 128 + (112.439 * r - 94.154 * g - 18.285 * b) / 256;
                }
                U[y * W / 2 + x] = u8(std::lround(cb / 4));
                V[y * W / 2 + x] = u8(std::lround(cr / 4));
            }
        th_ycbcr_buffer buf;
        buf[0] = {W, H, W, Y.data()};
        buf[1] = {W / 2, H / 2, W / 2, U.data()};
        buf[2] = {W / 2, H / 2, W / 2, V.data()};
        if (th_encode_ycbcr_in(enc, buf) != 0) break;
        while (th_encode_packetout(enc, f == N - 1, &op) > 0) ogg_stream_packetin(&os, &op);
        drain(false);
    }
    drain(true);
    ogg_stream_clear(&os);
    th_encode_free(enc);
    return out;
}

double psnr(const std::vector<u32>& px, int f) {
    double se = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int r, g, b;
            sourceRgb(f, x, y, r, g, b);
            const u32 p = px[std::size_t(y * W + x)];
            const double dr = double(p & 255) - r, dg = double((p >> 8) & 255) - g, db = double((p >> 16) & 255) - b;
            se += dr * dr + dg * dg + db * db;
        }
    const double mse = se / (3.0 * W * H);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}
void step(UiContext& ui) { ui.update(1.f / 60.f); }
} // namespace

int main(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;

    std::printf("%s\n", video::theoraVersion().c_str());
    const std::vector<u8> clip = encodeClip();
    UI_CHECK(clip.size() > 1000u);
    if (!out.empty()) { std::ofstream(out + "/fuse_ui_test.ogv", std::ios::binary).write(reinterpret_cast<const char*>(clip.data()), std::streamsize(clip.size())); }

    video::TheoraVideo v;
    std::string err;
    UI_CHECK(v.open(clip, &err));
    if (!err.empty()) std::printf("open: %s\n", err.c_str());
    UI_CHECK_EQ(v.frameCount(), u32(N));
    UI_CHECK(v.width() == u32(W) && v.height() == u32(H));
    UI_CHECK(std::abs(v.fps() - 24.f) < 1e-4f);
    for (u32 i = 0; i < u32(N); ++i) UI_CHECK_EQ(v.isKeyframe(i), i % 12 == 0);

    // sequential decode + quality
    std::vector<std::vector<u32>> seq(N, std::vector<u32>(std::size_t(W * H)));
    double worst = 99.0;
    for (int f = 0; f < N; ++f) {
        UI_CHECK(v.decode(u32(f), seq[std::size_t(f)].data()));
        worst = std::min(worst, psnr(seq[std::size_t(f)], f));
    }
    std::printf("worst PSNR %.2f dB over %d frames, %zu bytes\n", worst, N, clip.size());
    UI_CHECK(worst > 30.0);
    UI_CHECK_EQ(v.packetsDecoded(), u64(N));

    // random access
    std::vector<u32> px(std::size_t(W * H));
    u64 before = v.packetsDecoded();
    UI_CHECK(v.decode(30, px.data()));
    UI_CHECK_EQ(v.packetsDecoded() - before, u64(30 - 24 + 1));
    UI_CHECK(px == seq[30]);
    before = v.packetsDecoded();
    UI_CHECK(v.decode(31, px.data()));
    UI_CHECK_EQ(v.packetsDecoded() - before, 1u);
    UI_CHECK(px == seq[31]);
    before = v.packetsDecoded();
    UI_CHECK(v.decode(31, px.data())); // same frame: no packets
    UI_CHECK_EQ(v.packetsDecoded() - before, 0u);
    UI_CHECK(v.decode(5, px.data()));
    UI_CHECK(px == seq[5]);
    UI_CHECK(v.decode(47, px.data()));
    UI_CHECK(px == seq[47]);
    UI_CHECK(!v.decode(48, px.data()));

    // bad input
    video::TheoraVideo bad;
    UI_CHECK(!bad.open(std::vector<u8>(4096, 0x5a), &err) && !err.empty());
    UI_CHECK(!bad.open(std::vector<u8>(clip.begin(), clip.begin() + 40), &err));
    UI_CHECK(!bad.valid());

    // the widget plays it
    UiContext ui{{.viewport = {420, 220}, .font = &font()}};
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    ui.setCss(col, "gap", "6px");
    w::label(ui, col, "Theora " + std::to_string(v.width()) + "x" + std::to_string(v.height()) + " @ 24 fps, " + std::to_string(v.frameCount()) + " frames");
    const NodeHandle vw = w::videoWidget(ui, col, &v.source(), "video");
    w::videoSetImageIndex(ui, vw, 0);
    step(ui);
    UI_CHECK(w::videoImage(ui, vw)->width == W && w::videoImage(ui, vw)->height == H);
    UI_CHECK(std::memcmp(w::videoImage(ui, vw)->pixels.data(), seq[0].data(), seq[0].size() * 4) == 0);
    w::videoPlay(ui, vw, true);
    for (int i = 0; i < 24; ++i) ui.update(1.f / 24.f);
    const u32 at = w::videoFrame(ui, vw);
    UI_CHECK(at >= 22u && at <= 25u);
    w::videoPlay(ui, vw, false);
    w::videoSeek(ui, vw, 40);
    step(ui);
    UI_CHECK_EQ(w::videoFrame(ui, vw), 40u);
    UI_CHECK(std::memcmp(w::videoImage(ui, vw)->pixels.data(), seq[40].data(), seq[40].size() * 4) == 0);

    if (!out.empty()) {
        ui.update();
        Image img;
        img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.addImage(w::videoImage(ui, vw));
        r.render(ui.drawList(), img);
        const std::string path = out + "/theora_video.png";
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("wrote %s\n", path.c_str());
    }
    return finish("fuse_ui_theora_video");
}
