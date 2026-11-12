// VideoToolbox decoder (macOS / iOS). HARDWARE-UNTESTED (written without a Mac).
// Annex-B in; the first SPS/PPS (or VPS/SPS/PPS for HEVC) builds the format description; slices go in as AVCC samples.
#include <forge/vision/video.hpp>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>

namespace forge::vision::video {
namespace {
class vt final : public decoder {
public:
    vt(codec c, frame_cb cb) : c_(c), cb_(std::move(cb)) {}
    ~vt() override { if (s_) { VTDecompressionSessionInvalidate(s_); CFRelease(s_); } if (fmt_) CFRelease(fmt_); }
    const char* backend() const override { return "videotoolbox"; }
    bool decode(std::span<const std::uint8_t> d, std::int64_t pts, std::string* why) override {
        std::vector<std::uint8_t> avcc;
        bool hevc = c_ == codec::hevc;
        for (auto& n : split_annexb(d, c_)) {
            bool ps = hevc ? (n.type >= 32 && n.type <= 34) : (n.type == 7 || n.type == 8);
            if (ps) { params_[hevc ? n.type - 32 : n.type - 7].assign(n.bytes.begin(), n.bytes.end()); continue; }
            std::uint32_t l = std::uint32_t(n.bytes.size());
            std::uint8_t h[4] = {std::uint8_t(l >> 24), std::uint8_t(l >> 16), std::uint8_t(l >> 8), std::uint8_t(l)};
            avcc.insert(avcc.end(), h, h + 4); avcc.insert(avcc.end(), n.bytes.begin(), n.bytes.end());
        }
        if (!s_ && !start(why)) return params_[0].empty();   // keep feeding until parameter sets arrive
        if (avcc.empty()) return true;
        CMBlockBufferRef bb = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, avcc.size(), nullptr, nullptr, 0, avcc.size(), 0, &bb) != noErr) return false;
        CMBlockBufferReplaceDataBytes(avcc.data(), bb, 0, avcc.size());
        CMSampleBufferRef sb = nullptr; size_t sz = avcc.size();
        CMSampleTimingInfo t{kCMTimeInvalid, CMTimeMake(pts, 1000000), kCMTimeInvalid};
        OSStatus st = CMSampleBufferCreateReady(nullptr, bb, fmt_, 1, 1, &t, 1, &sz, &sb);
        CFRelease(bb);
        if (st != noErr) { if (why) *why = "videotoolbox: CMSampleBufferCreate failed"; return false; }
        st = VTDecompressionSessionDecodeFrame(s_, sb, 0, nullptr, nullptr);
        CFRelease(sb);
        if (st != noErr && why) *why = "videotoolbox: decode failed " + std::to_string(int(st));
        return st == noErr;
    }
    void flush() override { if (s_) VTDecompressionSessionWaitForAsynchronousFrames(s_); }
private:
    bool start(std::string* why) {
        bool hevc = c_ == codec::hevc;
        int need = hevc ? 3 : 2;
        for (int i = 0; i < need; ++i) if (params_[i].empty()) return false;
        const std::uint8_t* p[3]; size_t l[3];
        for (int i = 0; i < need; ++i) { p[i] = params_[i].data(); l[i] = params_[i].size(); }
        OSStatus st = hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, 3, p, l, 4, nullptr, &fmt_)
                           : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, p, l, 4, &fmt_);
        if (st != noErr) { if (why) *why = "videotoolbox: bad parameter sets"; return false; }
        NSDictionary* attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
        VTDecompressionOutputCallbackRecord cb{&vt::out, this};
        st = VTDecompressionSessionCreate(nullptr, fmt_, nullptr, (__bridge CFDictionaryRef)attrs, &cb, &s_);
        if (st != noErr) { if (why) *why = "videotoolbox: session create failed " + std::to_string(int(st)); return false; }
        return true;
    }
    static void out(void* self, void*, OSStatus st, VTDecodeInfoFlags, CVImageBufferRef img, CMTime pts, CMTime) {
        if (st != noErr || !img) return;
        auto* me = static_cast<vt*>(self);
        CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        frame f; f.width = int(CVPixelBufferGetWidth(img)); f.height = int(CVPixelBufferGetHeight(img));
        f.pts = pts.value * 1000000 / std::max<std::int64_t>(pts.timescale, 1);
        f.y.resize(std::size_t(f.width) * f.height); f.uv.resize(std::size_t(f.width) * (f.height / 2));
        for (int pl = 0; pl < 2; ++pl) {
            auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(img, pl));
            std::size_t stride = CVPixelBufferGetBytesPerRowOfPlane(img, pl); int rows = pl ? f.height / 2 : f.height;
            auto& dst = pl ? f.uv : f.y;
            for (int r = 0; r < rows; ++r) std::memcpy(&dst[std::size_t(r) * f.width], base + r * stride, std::size_t(f.width));
        }
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        me->cb_(std::move(f));
    }
    codec c_; frame_cb cb_; std::vector<std::uint8_t> params_[3]; CMVideoFormatDescriptionRef fmt_ = nullptr; VTDecompressionSessionRef s_ = nullptr;
};
} // namespace
std::unique_ptr<decoder> open_videotoolbox(codec c, frame_cb cb, std::string* why) {
    if (c != codec::h264 && c != codec::hevc) { if (why) *why = "videotoolbox: only h264/hevc wired"; return nullptr; }
    return std::make_unique<vt>(c, std::move(cb));
}
} // namespace forge::vision::video
