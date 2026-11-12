// UI7 UiSurface as an ECS component (in tree, fuse_ui_game_ecs): world-space / diegetic panels attached to entities.
// The panel's centre and axes come from the entity's Transform::local_to_world (column 0 = right, column 1 = up,
// their lengths scale the panel), the size from the component. UiSurfaceSystem updates and re-rasterises the panels
// that changed, routes pointer rays to the nearest panel that is not hidden behind scene geometry, and composites
// the panels into a frame with a per-pixel depth test against the scene depth (or always on top).
#pragma once

#include "fuse/ui/game/game_ui.hpp"

#include <fuse/ecs/registry.hpp>

#include <array>
#include <optional>
#include <vector>

namespace fuse::ui::game {

using Mat4 = std::array<f32, 16>; ///< column-major

struct UiSurfaceComponent {
    static constexpr const char* component_name = "UiSurface";
    UiSurface* surface = nullptr; ///< non-owning; the game owns the UiContext and its texture
    f32 width = 1.f, height = 1.f; ///< world units before the transform scale
    bool depthTest = true;        ///< false: always on top of the scene (still sorted among panels)
    bool interactive = true;      ///< receives ray pointer input
    f32 opacity = 1.f;
};

struct SurfaceHit {
    fuse::ecs::EntityID entity;
    Vec2 pixel;
    f32 t = 0.f;
};

class UiSurfaceSystem {
public:
    /// Places every surface from its transform and re-rasterises the ones whose UI changed. Returns renders done.
    u32 update(fuse::ecs::Registry& reg, f32 dt);
    /// Nearest interactive panel along the ray (dir need not be normalised; distances are world units). `sceneT` is the distance to the first scene surface along the same ray
    /// (from a depth buffer or physics); depth-tested panels behind it are occluded.
    std::optional<SurfaceHit> raycast(fuse::ecs::Registry& reg, Vec3 origin, Vec3 dir, f32 sceneT = 1e30f) const;
    /// Routes a ray pointer: the hit panel gets move / down / up, every other panel loses hover (and a press held on
    /// it is released off-panel, so no click). Returns the entity hit, if any.
    std::optional<fuse::ecs::EntityID> pointer(fuse::ecs::Registry& reg, Vec3 origin, Vec3 dir, bool down, bool up,
                                               f32 sceneT = 1e30f);
    /// CPU reference renderer: draws the panels over `color` (premultiplied RGBA8, width x height) for the camera
    /// `viewProj` / `eye`, nearest-texel sampling, back-to-front, with a per-pixel depth test against `depth` (ray
    /// distance from the eye per pixel, same size as color; empty = no scene depth). Returns pixels written.
    u64 composite(fuse::ecs::Registry& reg, const Mat4& viewProj, Vec3 eye, Image& color,
                  const std::vector<f32>& depth) const;
};

/// Inverse of a column-major 4x4 (identity when singular).
Mat4 inverse(const Mat4& m);

} // namespace fuse::ui::game
