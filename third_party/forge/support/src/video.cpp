// Hardware video decoder backends. HARDWARE-UNTESTED: written against the vendor headers' documented APIs.
#include <forge/vision/video.hpp>
#include <algorithm>
#include <deque>
#include <forge/core/dl.hpp>
#include <forge/os/os.hpp>
#if defined(FORGE_HAVE_NVCUVID)
#include <cuda.h>
#include <nvcuvid.h>
#endif
#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

namespace forge::vision::video {
#if defined(__APPLE__)
std::unique_ptr<decoder> open_videotoolbox(codec c, frame_cb cb, std::string* why);   // video_vt.mm
#endif

#if defined(FORGE_HAVE_NVCUVID)
// ---- NVDEC: cuvid parser drives sequence/decode/display callbacks; frames copied DtoH as NV12 ----
namespace nv {
struct api {
    os::library cu, cv;
    decltype(&cuInit) Init; decltype(&cuDeviceGet) DeviceGet; decltype(&cuCtxCreate) CtxCreate; decltype(&cuCtxDestroy) CtxDestroy;
    decltype(&cuCtxPushCurrent) Push; decltype(&cuCtxPopCurrent) Pop; decltype(&cuMemcpy2D) Memcpy2D;
    decltype(&cuvidCreateVideoParser) CreateParser; decltype(&cuvidParseVideoData) Parse; decltype(&cuvidDestroyVideoParser) DestroyParser;
    decltype(&cuvidCreateDecoder) CreateDecoder; decltype(&cuvidDestroyDecoder) DestroyDecoder; decltype(&cuvidDecodePicture) DecodePicture;
    decltype(&cuvidMapVideoFrame64) Map; decltype(&cuvidUnmapVideoFrame64) Unmap;
    bool load(std::string* why) {
        cu = os::library::open(FORGE_DLNAME("libcuda.so.1", "nvcuda.dll")).value);
        cv = os::library::open(FORGE_DLNAME("libnvcuvid.so.1", "nvcuvid.dll")).value);
        if (!cu.valid() || !cv.valid()) { if (why) *why = "nvdec: libcuda/libnvcuvid not found"; return false; }
#define L(h, f, s) f = h.get<decltype(f)>(s); if (!f) { if (why) *why = std::string("nvdec: missing ") + s; return false; }
        L(cu, Init, "cuInit") L(cu, DeviceGet, "cuDeviceGet") L(cu, CtxCreate, "cuCtxCreate_v2") L(cu, CtxDestroy, "cuCtxDestroy_v2")
        L(cu, Push, "cuCtxPushCurrent_v2") L(cu, Pop, "cuCtxPopCurrent_v2") L(cu, Memcpy2D, "cuMemcpy2D_v2")
        L(cv, CreateParser, "cuvidCreateVideoParser") L(cv, Parse, "cuvidParseVideoData") L(cv, DestroyParser, "cuvidDestroyVideoParser")
        L(cv, CreateDecoder, "cuvidCreateDecoder") L(cv, DestroyDecoder, "cuvidDestroyDecoder") L(cv, DecodePicture, "cuvidDecodePicture")
        L(cv, Map, "cuvidMapVideoFrame64") L(cv, Unmap, "cuvidUnmapVideoFrame64")
#undef L
        return true;
    }
};
class dec final : public decoder {
public:
    dec(codec c, frame_cb cb) : c_(c), cb_(std::move(cb)) {}
    bool init(std::string* why) {
        if (!a_.load(why)) return false;
        CUdevice d; if (a_.Init(0) || a_.DeviceGet(&d, 0) || a_.CtxCreate(&ctx_, 0, d)) { if (why) *why = "nvdec: no CUDA device"; return false; }
        CUVIDPARSERPARAMS p{};
        p.CodecType = c_ == codec::h264 ? cudaVideoCodec_H264 : c_ == codec::hevc ? cudaVideoCodec_HEVC : c_ == codec::vp9 ? cudaVideoCodec_VP9 : cudaVideoCodec_AV1;
        p.ulMaxNumDecodeSurfaces = 1; p.ulMaxDisplayDelay = 0; p.pUserData = this;
        p.pfnSequenceCallback = [](void* u, CUVIDEOFORMAT* f) { return static_cast<dec*>(u)->on_seq(f); };
        p.pfnDecodePicture = [](void* u, CUVIDPICPARAMS* pp) { auto* s = static_cast<dec*>(u); return s->a_.DecodePicture(s->dec_, pp) == CUDA_SUCCESS ? 1 : 0; };
        p.pfnDisplayPicture = [](void* u, CUVIDPARSERDISPINFO* di) { return static_cast<dec*>(u)->on_display(di); };
        if (a_.CreateParser(&parser_, &p)) { if (why) *why = "nvdec: cuvidCreateVideoParser failed"; return false; }
        return true;
    }
    ~dec() override {
        if (parser_) a_.DestroyParser(parser_);
        if (dec_) a_.DestroyDecoder(dec_);
        if (ctx_) a_.CtxDestroy(ctx_);
    }
    const char* backend() const override { return "nvdec"; }
    bool decode(std::span<const std::uint8_t> d, std::int64_t pts, std::string* why) override {
        CUVIDSOURCEDATAPACKET pk{}; pk.payload = d.data(); pk.payload_size = static_cast<unsigned long>(d.size());
        pk.flags = CUVID_PKT_TIMESTAMP; pk.timestamp = pts;
        a_.Push(ctx_); auto r = a_.Parse(parser_, &pk); CUcontext dummy; a_.Pop(&dummy);
        if (r != CUDA_SUCCESS && why) *why = "nvdec: cuvidParseVideoData failed";
        return r == CUDA_SUCCESS;
    }
    void flush() override { CUVIDSOURCEDATAPACKET pk{}; pk.flags = CUVID_PKT_ENDOFSTREAM; a_.Push(ctx_); a_.Parse(parser_, &pk); CUcontext d; a_.Pop(&d); }
private:
    int on_seq(CUVIDEOFORMAT* f) {
        if (dec_) { a_.DestroyDecoder(dec_); dec_ = nullptr; }
        CUVIDDECODECREATEINFO ci{};
        ci.CodecType = f->codec; ci.ChromaFormat = f->chroma_format; ci.OutputFormat = cudaVideoSurfaceFormat_NV12;
        ci.bitDepthMinus8 = f->bit_depth_luma_minus8; ci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
        ci.ulWidth = f->coded_width; ci.ulHeight = f->coded_height; ci.ulMaxWidth = f->coded_width; ci.ulMaxHeight = f->coded_height;
        w_ = f->display_area.right - f->display_area.left; h_ = f->display_area.bottom - f->display_area.top;
        ci.ulTargetWidth = w_; ci.ulTargetHeight = h_;
        ci.ulNumDecodeSurfaces = std::max<unsigned>(f->min_num_decode_surfaces, 8); ci.ulNumOutputSurfaces = 2;
        ci.display_area.left = short(f->display_area.left); ci.display_area.top = short(f->display_area.top);
        ci.display_area.right = short(f->display_area.right); ci.display_area.bottom = short(f->display_area.bottom);
        if (a_.CreateDecoder(&dec_, &ci)) return 0;
        return int(ci.ulNumDecodeSurfaces);
    }
    int on_display(CUVIDPARSERDISPINFO* di) {
        CUVIDPROCPARAMS pp{}; pp.progressive_frame = di->progressive_frame; pp.top_field_first = di->top_field_first;
        unsigned long long dp = 0; unsigned pitch = 0;
        if (a_.Map(dec_, di->picture_index, &dp, &pitch, &pp)) return 0;
        frame fr; fr.width = int(w_); fr.height = int(h_); fr.pts = di->timestamp;
        fr.y.resize(std::size_t(w_) * h_); fr.uv.resize(std::size_t(w_) * (h_ / 2));
        auto copy = [&](CUdeviceptr src, std::uint8_t* dst, unsigned rows) {
            CUDA_MEMCPY2D m{}; m.srcMemoryType = CU_MEMORYTYPE_DEVICE; m.srcDevice = src; m.srcPitch = pitch;
            m.dstMemoryType = CU_MEMORYTYPE_HOST; m.dstHost = dst; m.dstPitch = w_; m.WidthInBytes = w_; m.Height = rows; a_.Memcpy2D(&m); };
        copy(CUdeviceptr(dp), fr.y.data(), h_);
        copy(CUdeviceptr(dp + std::size_t(pitch) * ((h_ + 1) & ~1u)), fr.uv.data(), h_ / 2);   // chroma follows the luma plane (aligned height)
        a_.Unmap(dec_, dp);
        cb_(std::move(fr));
        return 1;
    }
    codec c_; frame_cb cb_; api a_{}; CUcontext ctx_ = nullptr; CUvideoparser parser_ = nullptr; CUvideodecoder dec_ = nullptr; unsigned w_ = 0, h_ = 0;
};
} // namespace nv
#endif

#if defined(__ANDROID__)
// ---- MediaCodec (NDK): Annex-B in, NV12 (COLOR_FormatYUV420SemiPlanar) out; csd from the first SPS/PPS ----
class mediacodec final : public decoder {
public:
    mediacodec(codec c, frame_cb cb) : c_(c), cb_(std::move(cb)) {}
    ~mediacodec() override { if (mc_) { AMediaCodec_stop(mc_); AMediaCodec_delete(mc_); } }
    const char* backend() const override { return "mediacodec"; }
    bool decode(std::span<const std::uint8_t> d, std::int64_t pts, std::string* why) override {
        if (!mc_ && !start(d, why)) return pending(d, pts);
        auto i = AMediaCodec_dequeueInputBuffer(mc_, 10000);
        if (i < 0) { if (why) *why = "mediacodec: no input buffer"; return false; }
        std::size_t cap = 0; auto* buf = AMediaCodec_getInputBuffer(mc_, std::size_t(i), &cap);
        std::size_t n = std::min(cap, d.size()); std::memcpy(buf, d.data(), n);
        AMediaCodec_queueInputBuffer(mc_, std::size_t(i), 0, n, std::uint64_t(pts), 0);
        drain(0); return true;
    }
    void flush() override {
        if (!mc_) return;
        auto i = AMediaCodec_dequeueInputBuffer(mc_, 10000);
        if (i >= 0) AMediaCodec_queueInputBuffer(mc_, std::size_t(i), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        drain(100000);
    }
private:
    bool pending(std::span<const std::uint8_t>, std::int64_t) { return true; }   // wait for SPS before configuring
    bool start(std::span<const std::uint8_t> d, std::string* why) {
        int w = 0, h = 0;
        for (auto& n : split_annexb(d, c_)) if (c_ == codec::h264 && n.type == 7) h264_sps_size(n.bytes, w, h);
        if (c_ == codec::h264 && (!w || !h)) return false;
        const char* mime = c_ == codec::h264 ? "video/avc" : c_ == codec::hevc ? "video/hevc" : c_ == codec::vp9 ? "video/x-vnd.on2.vp9" : "video/av01";
        mc_ = AMediaCodec_createDecoderByType(mime);
        if (!mc_) { if (why) *why = std::string("mediacodec: no decoder for ") + mime; return false; }
        auto* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w ? w : 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h ? h : 1080);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21 /* YUV420SemiPlanar */);
        auto st = AMediaCodec_configure(mc_, f, nullptr, nullptr, 0); AMediaFormat_delete(f);
        if (st != AMEDIA_OK || AMediaCodec_start(mc_) != AMEDIA_OK) { if (why) *why = "mediacodec: configure/start failed"; AMediaCodec_delete(mc_); mc_ = nullptr; return false; }
        w_ = w; h_ = h; return true;
    }
    void drain(std::int64_t timeout_us) {
        for (;;) {
            AMediaCodecBufferInfo info;
            auto o = AMediaCodec_dequeueOutputBuffer(mc_, &info, timeout_us);
            if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                auto* f = AMediaCodec_getOutputFormat(mc_);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w_); AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h_);
                AMediaFormat_getInt32(f, "stride", &stride_); AMediaFormat_getInt32(f, "slice-height", &slice_);
                AMediaFormat_delete(f); continue;
            }
            if (o < 0) return;
            std::size_t sz = 0; auto* b = AMediaCodec_getOutputBuffer(mc_, std::size_t(o), &sz);
            if (b && info.size > 0) {
                int st = stride_ > 0 ? stride_ : w_, sl = slice_ > 0 ? slice_ : h_;
                frame fr; fr.width = w_; fr.height = h_; fr.pts = info.presentationTimeUs;
                fr.y.resize(std::size_t(w_) * h_); fr.uv.resize(std::size_t(w_) * (h_ / 2));
                const std::uint8_t* base = b + info.offset;
                for (int r = 0; r < h_; ++r) std::memcpy(&fr.y[std::size_t(r) * w_], base + std::size_t(r) * st, std::size_t(w_));
                for (int r = 0; r < h_ / 2; ++r) std::memcpy(&fr.uv[std::size_t(r) * w_], base + std::size_t(st) * sl + std::size_t(r) * st, std::size_t(w_));
                cb_(std::move(fr));
            }
            AMediaCodec_releaseOutputBuffer(mc_, std::size_t(o), false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return;
        }
    }
    codec c_; frame_cb cb_; AMediaCodec* mc_ = nullptr; std::int32_t w_ = 0, h_ = 0, stride_ = 0, slice_ = 0;
};
#endif

std::unique_ptr<decoder> open_decoder(codec c, frame_cb cb, const std::string& backend, std::string* why) {
    std::string tried;
    auto want = [&](const char* b) { return backend == "auto" || backend == b; };
#if defined(FORGE_HAVE_NVCUVID)
    if (want("nvdec")) { auto d = std::make_unique<nv::dec>(c, cb); std::string e; if (d->init(&e)) return d; tried += e + "; "; }
#else
    if (backend == "nvdec") tried += "nvdec: built without nvcuvid.h; ";
#endif
#if defined(__APPLE__)
    if (want("videotoolbox")) { std::string e; if (auto d = open_videotoolbox(c, cb, &e)) return d; tried += e + "; "; }
#endif
#if defined(__ANDROID__)
    if (want("mediacodec")) return std::make_unique<mediacodec>(c, cb);
#endif
    if (want("vaapi")) {
        auto p = vaapi_probe();
        tried += p.ok ? "vaapi: device present (" + p.driver + ") but bitstream decode is not implemented; " : "vaapi: " + p.why + "; ";
    }
    if (why) *why = tried.empty() ? "no hardware decoder for " + std::string(codec_name(c)) : tried;
    return nullptr;
}
} // namespace forge::vision::video
