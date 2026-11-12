// docs/modelfile.md example: write a .fpk with two tensors, read it back, verify the hashes.
#include <forge/modelfile/fpk.hpp>
#include <cstdio>
#include <filesystem>
#include <vector>
using namespace forge;

int main() {
    auto path = (std::filesystem::temp_directory_path() / "doc_example_modelfile.fpk").string();
    std::vector<float> a(256);
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = float(i) * 0.01f;
    {
        modelfile::writer w;
        w.set_manifest(json::object{{"name", "demo"}});
        w.add_tensor("w", {16, 16}, a, "f32");
        w.add_tensor("wq", {256}, a, "q8_0");          // quantised on write
        w.write(path);
    }
    bool ok; std::vector<float> data;
    {   // scoped: the reader maps the file, and Windows cannot delete a mapped file
        modelfile::reader r(path);
        ok = r.verify().empty();                        // empty = every hash matches
        auto t = r.load("w");                           // decoded to f32
        data = t.data;
        std::printf("name=%s verify=%s w[255]=%g wq codec=%s\n", r.manifest()["name"].str().c_str(),
                    ok ? "ok" : "BAD", t.data[255], r.entry("wq").codec.c_str());
    }
    std::filesystem::remove(path);
    return ok && data == a ? 0 : 1;
}
