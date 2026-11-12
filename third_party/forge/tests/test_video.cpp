// Video decode: Annex-B / SPS parsing on CPU always; hardware decoders only when present (otherwise reports skip).
#include "check.hpp"
#include <forge/vision/video.hpp>
#include <cstdlib>
#include <fstream>
#include <iterator>
using namespace forge::vision::video;

// 1280x720 High-profile SPS (x264 default) with emulation-prevention free payload.
static const std::uint8_t sps720[] = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xbb, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc0, 0xf1, 0x83, 0x19, 0x60};

TEST(annexb_split) {
    std::vector<std::uint8_t> s = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 9, 9};
    auto n = split_annexb(s);
    CHECK(n.size() == 3);
    CHECK(n[0].type == 7 && n[0].bytes.size() == 3);
    CHECK(n[1].type == 8 && n[1].bytes.size() == 2);
    CHECK(n[2].type == 5 && n[2].bytes.size() == 3);
    auto hevc = split_annexb(std::vector<std::uint8_t>{0, 0, 1, 0x40, 1, 0, 0, 1, 0x42, 1}, codec::hevc);
    CHECK(hevc.size() == 2 && hevc[0].type == 32 && hevc[1].type == 33);
}
TEST(avcc_to_annexb) {
    std::vector<std::uint8_t> a = {0, 0, 0, 2, 0x65, 7, 0, 0, 0, 1, 0x41};
    auto b = length_prefixed_to_annexb(a);
    auto n = split_annexb(b);
    CHECK(n.size() == 2 && n[0].type == 5 && n[1].type == 1);
}
TEST(sps_dimensions) {
    int w = 0, h = 0;
    CHECK(h264_sps_size(sps720, w, h));
    CHECK_MSG(w == 1280 && h == 720, "got %dx%d", w, h);
}
TEST(vaapi_probe_or_skip) {
    auto p = vaapi_probe();
    if (!p.ok) { std::printf("  skip: %s\n", p.why.c_str()); return; }
    std::printf("  vaapi %s: %zu decodable codecs\n", p.driver.c_str(), p.decodable.size());
}
TEST(hw_decode_or_skip) {   // FORGE_TEST_H264=<annex-b file> on a machine with NVDEC / VideoToolbox / MediaCodec
    int frames = 0; std::string why;
    auto d = open_decoder(codec::h264, [&](frame&& f) { CHECK(f.width > 0 && f.y.size() == std::size_t(f.width) * f.height); ++frames; }, "auto", &why);
    if (!d) { std::printf("  skip: %s\n", why.c_str()); return; }
    auto* path = std::getenv("FORGE_TEST_H264");
    if (!path) { std::printf("  %s opened; skip decode: FORGE_TEST_H264 not set\n", d->backend()); return; }
    std::ifstream f(path, std::ios::binary); std::vector<std::uint8_t> s((std::istreambuf_iterator<char>(f)), {});
    CHECK(d->decode(s, 0, &why)); d->flush();
    CHECK(frames > 0);
}
