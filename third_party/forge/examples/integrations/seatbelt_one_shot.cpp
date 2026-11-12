// Seatbelt / phone detection (Part F.2): `one_shot` session with a fixed latency budget; vision pre/post.
// Usage: seatbelt_one_shot [detector.fpk] [budget_ms]. Without a model the detector is a synthetic stub so
// the pre/post-processing and the budget accounting still run.
#include <forge/inference/inference.hpp>
#include <forge/vision/vision.hpp>
#include <chrono>
#include <cstdio>

using namespace forge;

int main(int argc, char** argv) {
    double budget_ms = argc > 2 ? std::atof(argv[2]) : 20.0;
    vision::image cam{240, 320, 3, std::vector<float>(240 * 320 * 3, 0.5f)};
    for (std::size_t y = 80; y < 160; ++y) for (std::size_t x = 100; x < 140; ++x) cam.at(y, x, 0) = 1.f;   // "belt" stripe
    std::shared_ptr<inference::session> s;
    if (argc > 1) s = std::make_shared<inference::session>(argv[1]);
    int over = 0, frames = 10;
    for (int f = 0; f < frames; ++f) {
        auto t0 = std::chrono::steady_clock::now();
        auto in = vision::resize_bilinear(cam, 96, 96);                // preprocessor
        std::vector<vision::box> boxes; std::vector<float> score;
        if (s) {
            auto out = s->infer({{s->blueprint().inputs.at(0).name, in.px}});
            auto& o = out.begin()->second;                             // [n, 5]: x0 y0 x1 y1 score
            for (std::size_t i = 0; i + 4 < o.size(); i += 5) { boxes.push_back({o[i], o[i + 1], o[i + 2], o[i + 3]}); score.push_back(o[i + 4]); }
        } else {
            boxes = {{100, 80, 140, 160}, {102, 82, 141, 158}, {10, 10, 30, 30}}; score = {0.9f, 0.8f, 0.2f};
        }
        auto keep = vision::nms(boxes, score, 0.5f);                    // postprocessor
        bool belt = false; for (auto k : keep) belt |= score[k] > 0.5f;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > budget_ms) ++over;
        if (f == 0) std::printf("seatbelt: %s, %zu box(es) after NMS, %.2f ms (budget %.1f)\n", belt ? "worn" : "NOT worn", keep.size(), ms, budget_ms);
    }
    std::printf("seatbelt: %d/%d frames over budget\n", over, frames);
    return 0;
}
