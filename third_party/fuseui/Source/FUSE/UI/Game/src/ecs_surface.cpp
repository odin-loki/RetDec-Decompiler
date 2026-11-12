// UI7 UiSurface ECS component (see ecs_surface.hpp).
#include "fuse/ui/game/ecs_surface.hpp"

#include <fuse/ecs/components/transform.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ui::game {
namespace {
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;

Vec3 col(const Transform& t, int c) { return {t.local_to_world.data[c * 4 + 0], t.local_to_world.data[c * 4 + 1], t.local_to_world.data[c * 4 + 2]}; }
f32 len(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 norm(Vec3 v) { const f32 l = len(v); return l > 0.f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{}; }

void place(const Transform& t, const UiSurfaceComponent& c) {
    UiSurface& s = *c.surface;
    const Vec3 r = col(t, 0), u = col(t, 1);
    s.center = col(t, 3);
    s.right = norm(r);
    s.up = norm(u);
    s.width = c.width * len(r);
    s.height = c.height * len(u);
    s.depthTest = c.depthTest;
}
u8 ch(u32 p, int i) { return u8(p >> (8 * i)); }
} // namespace

Mat4 inverse(const Mat4& m) {
    // cofactor expansion (MESA gluInvertMatrix), column-major in and out
    Mat4 inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (f32& v : inv) v /= det;
    return inv;
}

u32 UiSurfaceSystem::update(Registry& reg, f32 dt) {
    u32 renders = 0;
    reg.each<Transform, UiSurfaceComponent>([&](EntityID, Transform& t, UiSurfaceComponent& c) {
        if (!c.surface || !c.surface->ui) return;
        place(t, c);
        renders += surfaceRender(*c.surface, dt) ? 1u : 0u;
    });
    return renders;
}

std::optional<SurfaceHit> UiSurfaceSystem::raycast(Registry& reg, Vec3 o, Vec3 d, f32 sceneT) const {
    d = norm(d); // distances (sceneT, hit t) are world units
    std::optional<SurfaceHit> best;
    reg.each<Transform, UiSurfaceComponent>([&](EntityID e, Transform& t, UiSurfaceComponent& c) {
        if (!c.surface || !c.surface->ui || !c.interactive) return;
        place(t, c);
        f32 hitT = 0.f;
        const auto px = surfaceRaycast(*c.surface, o, d, &hitT);
        if (!px || (c.depthTest && hitT > sceneT)) return;
        // always-on-top panels win over depth-tested ones (they draw over them), then the nearest
        const bool top = !c.depthTest;
        if (!best) { best = SurfaceHit{e, *px, top ? -1.f / (1.f + hitT) : hitT}; return; }
        const f32 key = top ? -1.f / (1.f + hitT) : hitT;
        if (key < best->t) best = SurfaceHit{e, *px, key};
    });
    if (best) { // report the real distance
        auto* c = reg.get<UiSurfaceComponent>(best->entity);
        f32 hitT = 0.f;
        (void)surfaceRaycast(*c->surface, o, d, &hitT);
        best->t = hitT;
    }
    return best;
}

std::optional<EntityID> UiSurfaceSystem::pointer(Registry& reg, Vec3 o, Vec3 d, bool down, bool up, f32 sceneT) {
    const auto hit = raycast(reg, o, d, sceneT);
    reg.each<Transform, UiSurfaceComponent>([&](EntityID e, Transform&, UiSurfaceComponent& c) {
        if (!c.surface || !c.surface->ui) return;
        UiContext& ui = *c.surface->ui;
        if (hit && hit->entity == e) {
            ui.pointerMove(hit->pixel);
            if (down) ui.pointerDown(hit->pixel);
            if (up) ui.pointerUp(hit->pixel);
        } else {
            ui.pointerMove({-1.f, -1.f});
            if (up && ui.pressed().valid()) ui.pointerUp({-1.f, -1.f});
        }
    });
    if (!hit) return std::nullopt;
    return hit->entity;
}

u64 UiSurfaceSystem::composite(Registry& reg, const Mat4& viewProj, Vec3 eye, Image& color, const std::vector<f32>& depth) const {
    struct Item { const UiSurfaceComponent* c; f32 dist; };
    std::vector<Item> items;
    reg.each<Transform, UiSurfaceComponent>([&](EntityID, Transform& t, UiSurfaceComponent& c) {
        if (!c.surface || c.surface->texture.width == 0) return;
        place(t, c);
        const Vec3 p = c.surface->center;
        items.push_back({&c, std::hypot(p.x - eye.x, p.y - eye.y, p.z - eye.z)});
    });
    // back to front; always-on-top panels after every depth-tested one
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.c->depthTest != b.c->depthTest) return a.c->depthTest;
        return a.dist > b.dist;
    });
    const Mat4 inv = inverse(viewProj);
    const int W = color.width, H = color.height;
    const bool haveDepth = depth.size() == std::size_t(W) * std::size_t(H);
    u64 written = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const f32 nx = (f32(x) + 0.5f) / f32(W) * 2.f - 1.f, ny = 1.f - (f32(y) + 0.5f) / f32(H) * 2.f;
            auto unproject = [&](f32 z) {
                const f32 v[4] = {nx, ny, z, 1.f};
                f32 r[4];
                for (int i = 0; i < 4; ++i) r[i] = inv[i] * v[0] + inv[4 + i] * v[1] + inv[8 + i] * v[2] + inv[12 + i] * v[3];
                return Vec3{r[0] / r[3], r[1] / r[3], r[2] / r[3]};
            };
            const Vec3 far = unproject(1.f);
            const Vec3 dir = norm({far.x - eye.x, far.y - eye.y, far.z - eye.z});
            const f32 sceneT = haveDepth ? depth[std::size_t(y) * W + x] : 1e30f;
            u32& dst = color.pixels[std::size_t(y) * W + x];
            for (const Item& it : items) {
                const UiSurface& s = *it.c->surface;
                f32 t = 0.f;
                const auto px = surfaceRaycast(s, eye, dir, &t);
                if (!px || (it.c->depthTest && t > sceneT)) continue;
                const Size vp = s.ui->viewport();
                const int tx = std::clamp(int(px->x * f32(s.texture.width) / vp.w), 0, s.texture.width - 1);
                const int ty = std::clamp(int(px->y * f32(s.texture.height) / vp.h), 0, s.texture.height - 1);
                const u32 src = s.texture.at(tx, ty);
                const f32 a = f32(ch(src, 3)) / 255.f * it.c->opacity;
                if (a <= 0.f) continue;
                u32 out = 0;
                for (int c = 0; c < 4; ++c) {
                    const f32 sv = f32(ch(src, c)) * it.c->opacity, dv = f32(ch(dst, c));
                    out |= u32(std::lround(std::clamp(sv + dv * (1.f - a), 0.f, 255.f))) << (8 * c);
                }
                dst = out;
                ++written;
            }
        }
    }
    return written;
}

} // namespace fuse::ui::game
