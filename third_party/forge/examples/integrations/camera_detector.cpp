// CameraDetector (Part F.2): macro and micro stages under `diffusion_loop`; vision pre/post.
// The denoisers here are analytic stand-ins; in production each is a sub-graph of one blueprint.
#include <forge/inference/diffusion.hpp>
#include <forge/vision/vision.hpp>
#include <cmath>
#include <cstdio>

using namespace forge;
using namespace forge::inference::diffusion;

int main() {
    auto sch = noise_scheduler::make(1000);
    std::size_t n = 32 * 32;
    latent target(n); for (std::size_t i = 0; i < n; ++i) target[i] = std::sin(float(i) * 0.1f);
    // Macro stage: predicts the clean sample directly (coarse layout).
    forge::inference::diffusion::session macro(sch, [&](const latent&, double, int) { return target; });
    options om; om.steps = 8; om.pred = prediction::sample; om.kind = sampler_kind::ddim;
    latent coarse = macro.run(macro.initial_noise(n, 7), om);
    // Micro stage: refines from the coarse latent with a few low-noise steps.
    forge::inference::diffusion::session micro(sch, [&](const latent& x, double, int) { latent d(x.size()); for (std::size_t i = 0; i < x.size(); ++i) d[i] = target[i]; return d; });
    options mi = om; mi.steps = 8; mi.start_step = 5;
    latent fine = micro.run(coarse, mi);
    double err = 0; for (std::size_t i = 0; i < n; ++i) err = std::max(err, double(std::abs(fine[i] - target[i])));
    vision::image img{32, 32, 1, fine};
    auto up = vision::resize_bilinear(img, 64, 64);
    std::printf("camera_detector: macro+micro max err %.4f, output %zux%zu\n", err, up.h, up.w);
    return err < 1e-3 ? 0 : 1;
}
