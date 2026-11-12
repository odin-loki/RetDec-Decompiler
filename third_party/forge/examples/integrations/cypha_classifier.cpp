// Cypha integration (Part F.2): core + lane only for the classifier hot loop, data packs as .fpk sections,
// and an optional `custom` session plugin so apps reach Cypha through the inference API.
// Usage: cypha_classifier [modelfile.fpk]   (writes a demo modelfile to the temp dir when omitted)
#include <forge/simd/dispatch.hpp>
#include <forge/inference/plugin.hpp>
#include <forge/modelfile/fpk.hpp>
#include <cstdio>
#include <filesystem>
#include <sstream>

using namespace forge;

// --- Cypha side: depends only on core + lane --------------------------------------------------------
struct cypha_classifier {
    std::vector<std::string> labels; std::vector<float> centroids; std::size_t dim = 0;
    static cypha_classifier from_text(std::string_view stats) {     // "dim\nlabel v0 v1 ...\n..."
        cypha_classifier c; std::istringstream is{std::string(stats)}; is >> c.dim; std::string l;
        while (is >> l) { c.labels.push_back(l); for (std::size_t i = 0; i < c.dim; ++i) { float v; is >> v; c.centroids.push_back(v); } }
        return c;
    }
    std::vector<float> scores(const float* x) const {                // explicit SIMD dot on every target
        auto& k = simd::active(); std::vector<float> s(labels.size());
        for (std::size_t i = 0; i < labels.size(); ++i) s[i] = k.dot(x, centroids.data() + i * dim, dim);
        return s;
    }
};

// --- App side: register as a `custom` session plugin -----------------------------------------------
struct cypha_service : inference::plugin::service {
    cypha_classifier c;
    explicit cypha_service(const std::string& pack) {
        modelfile::reader r(pack);
        c = cypha_classifier::from_text(r.section_text("cypha.text_stats"));
    }
    std::string name() const override { return "cypha"; }
    inference::plugin::tensors run(const inference::plugin::tensors& in) override {
        auto it = in.find("features");
        if (it == in.end() || it->second.size() != c.dim) raise(errc::invalid_argument, "cypha: need 'features' of {} floats", c.dim);
        return {{"scores", c.scores(it->second.data())}};
    }
};
static inference::plugin::registrar reg{"cypha", [](const std::string& cfg) { return std::make_unique<cypha_service>(cfg); }};

int main(int argc, char** argv) {
    std::string path;
    if (argc > 1) path = argv[1];
    else {
        path = (std::filesystem::temp_directory_path() / "cypha_demo.fpk").string();
        modelfile::writer w;
        w.set_manifest(json::object{{"name", "cypha-demo"}, {"version", "1"}});
        w.set_section("cypha.text_stats", "4\nnews 1 0 0 0\nsport 0 1 0 0\ncode 0 0 1 1\n");
        w.set_section("cypha.dictionary", "a\nan\nthe\n");
        w.write(path);
    }
    inference::plugin::session s("cypha", path);
    auto out = s.infer({{"features", {0.1f, 0.2f, 0.9f, 0.8f}}});
    auto& sc = out["scores"];
    std::size_t best = 0; for (std::size_t i = 1; i < sc.size(); ++i) if (sc[i] > sc[best]) best = i;
    auto& svc = static_cast<cypha_service&>(s.get());
    std::printf("cypha (%s lane): %s (%.2f)\n", simd::active().target, svc.c.labels[best].c_str(), sc[best]);
    return svc.c.labels[best] == "code" || argc > 1 ? 0 : 1;
}
