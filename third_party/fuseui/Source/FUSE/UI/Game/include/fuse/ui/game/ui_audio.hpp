// UI7 theme sounds through fuse_audio (in tree, fuse_ui_game_audio): loads the clips a theme's @sounds block names
// (extractSoundTheme) and plays them as 2D one-shots when GameUi / GamepadNav report UI sounds.
#pragma once

#include "fuse/ui/game/game_extras.hpp"

#include <fuse/audio/audio_engine.hpp>

#include <array>
#include <string_view>

namespace fuse::ui::game {

class UiAudio {
public:
    explicit UiAudio(fuse::audio::AudioEngine& engine) : m_engine(engine) {}
    /// Loads every clip the theme names (paths relative to `baseDir`); returns the number that failed.
    u32 load(const SoundTheme& theme, std::string_view baseDir = {});
    /// Uses an in-memory clip for a sound (procedural / tests).
    void setClip(UiSound s, fuse::audio::AudioClip&& clip);
    bool has(UiSound s) const { return m_clips[u32(s)].isValid(); }
    void play(UiSound s);
    u32 played(UiSound s) const { return m_played[u32(s)]; }
    void setVolume(f32 v) { m_volume = v; }
    /// Routes `game`'s sound hook here.
    void attach(GameUi& game);
    static void hook(void* self, UiSound s) { static_cast<UiAudio*>(self)->play(s); }

private:
    fuse::audio::AudioEngine& m_engine;
    std::array<fuse::Handle<fuse::audio::AudioClip>, 6> m_clips{
        fuse::Handle<fuse::audio::AudioClip>::invalid(), fuse::Handle<fuse::audio::AudioClip>::invalid(),
        fuse::Handle<fuse::audio::AudioClip>::invalid(), fuse::Handle<fuse::audio::AudioClip>::invalid(),
        fuse::Handle<fuse::audio::AudioClip>::invalid(), fuse::Handle<fuse::audio::AudioClip>::invalid()};
    std::array<u32, 6> m_played{};
    f32 m_volume = 1.f;
};

} // namespace fuse::ui::game
