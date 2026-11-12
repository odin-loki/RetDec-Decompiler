// UI7 theme sounds through fuse_audio (see ui_audio.hpp).
#include "fuse/ui/game/ui_audio.hpp"

#include <string>

namespace fuse::ui::game {

u32 UiAudio::load(const SoundTheme& theme, std::string_view baseDir) {
    u32 failed = 0;
    m_volume = theme.volume;
    for (u32 i = 0; i < 6; ++i) {
        const std::string_view rel = theme.clips[i];
        if (rel.empty()) continue;
        std::string path(baseDir);
        if (!path.empty() && path.back() != '/') path.push_back('/');
        path.append(rel);
        const auto h = m_engine.load_clip(path.c_str());
        if (h.isValid()) m_clips[i] = h;
        else ++failed;
    }
    return failed;
}
void UiAudio::setClip(UiSound s, fuse::audio::AudioClip&& clip) { m_clips[u32(s)] = m_engine.register_clip(std::move(clip)); }
void UiAudio::play(UiSound s) {
    const auto h = m_clips[u32(s)];
    if (!h.isValid()) return;
    m_engine.play_2d(h, m_volume);
    ++m_played[u32(s)];
}
void UiAudio::attach(GameUi& game) { game.setSoundHook(&UiAudio::hook, this); }

} // namespace fuse::ui::game
