#pragma once
// fuse_ui_video: Theora (.ogv) playback source for widgets::videoWidget.
//
// TheoraVideo holds one Ogg Theora stream in memory (other streams in the file, e.g. Vorbis audio, are skipped)
// and serves frames by index: sequential playback decodes one packet per frame; a seek decodes forward from the
// nearest keyframe at or before the target. Output is opaque RGBA8 (R in the low byte), the frame's picture
// region, BT.601 video-range YCbCr 4:2:0 / 4:2:2 / 4:4:4 converted on the CPU.
//
//   video::TheoraVideo v;
//   if (v.openFile("intro.ogv")) w::videoWidget(ui, parent, &v.source());
//
// The TheoraVideo must outlive the widget (the source points at it).
#include "fuse/ui/widgets/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::video {

class TheoraVideo {
public:
    TheoraVideo();
    ~TheoraVideo();
    TheoraVideo(const TheoraVideo&) = delete;
    TheoraVideo& operator=(const TheoraVideo&) = delete;

    /// Parse an Ogg file held in memory; finds the first Theora stream and indexes its packets.
    bool open(std::vector<u8> ogg, std::string* error = nullptr);
    bool openFile(const std::string& path, std::string* error = nullptr);
    bool valid() const;

    u32 width() const;  ///< picture width
    u32 height() const; ///< picture height
    u32 frameCount() const;
    f32 fps() const;
    bool isKeyframe(u32 index) const;

    /// Decode frame `index` into `rgba` (width() * height() u32). False when out of range or on a decode error.
    bool decode(u32 index, u32* rgba);
    /// Packets fed to the decoder so far (seek cost probe for tests).
    u64 packetsDecoded() const;

    /// A VideoSource over this stream (valid while this object lives and stays open).
    const widgets::VideoSource& source() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/// Build-time info: "libtheora <version>" (the vendored decoder's own version string).
std::string theoraVersion();

} // namespace fuse::ui::video
