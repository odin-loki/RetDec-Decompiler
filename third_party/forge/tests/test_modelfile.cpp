#include "check.hpp"
#include <forge/inference/inference.hpp>
#include <cstdio>
#include <filesystem>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <fstream>
#include "tiny_model.hpp"
#include <cstring>
static std::string tmp(const std::string& n) { return (std::filesystem::temp_directory_path() / ("forge_test_pack_" + std::to_string(::getpid()) + "_" + n)).string(); }

using namespace forge;

TEST(blake3_matches_reference_implementation) {
    struct kat { std::size_t n; const char* hex; };
    static const kat k[] = {
        {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {64, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
        {3073, "7124b49501012f81cc7f11ca069ec9226cecb8a2c850cfe644e327d22d3e1cd3"},
        {100000, "d93c23eedaf165a7e0be908ba86f1a7a520d568d2d13cde787c8580c5c72cc54"},
    };
    for (auto& e : k) {
        std::vector<std::uint8_t> b(e.n); for (std::size_t i = 0; i < e.n; ++i) b[i] = std::uint8_t(i % 251);
        CHECK_MSG(hash::blake3::hex(hash::blake3::of(b)) == e.hex, "n=%zu", e.n);
        hash::blake3 h; std::size_t p = 0;  // streaming in odd pieces
        while (p < b.size()) { std::size_t t = std::min<std::size_t>(77, b.size() - p); h.update(b.data() + p, t); p += t; }
        CHECK(hash::blake3::hex(h.final()) == e.hex);
    }
}

TEST(json_roundtrip_and_errors) {
    auto v = json::parse(R"({"a": [1, 2.5, -3e2, true, null], "s": "x\"\u00e9\ud83d\ude00", "o": {}})");
    CHECK(v["a"][2].num() == -300); CHECK(v["s"].str() == "x\"\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(json::parse(json::dump(v))["a"][1].num() == 2.5);
    bool threw = false; try { json::parse("{\"a\": }"); } catch (const forge_exception&) { threw = true; } CHECK(threw);
}

TEST(fpk_write_read_verify_and_corruption) {
    auto path = std::filesystem::temp_directory_path() / ("forge_test_" + std::to_string(::getpid()) + ".fpk");
    std::vector<float> a(1000); for (std::size_t i = 0; i < a.size(); ++i) a[i] = float(i) * 0.01f - 3.f;
    {
        modelfile::writer w;
        w.set_manifest(json::object{{"k", "v"}});
        w.set_section("blueprint", "blueprint \"x\" graph { y = x } output y : f32[1]");
        w.add_tensor("a", {10, 100}, a, "f32");
        w.add_tensor("b", {1000}, a, "q8_0");
        w.add_tensor("c", {7}, std::span<const float>(a.data(), 7), "q4_k");  // falls back to f16
        w.write(path.string());
    }
    {
        modelfile::reader r(path.string());
        CHECK(r.manifest()["k"].str() == "v");
        CHECK(r.verify().empty());
        auto t = r.load("a"); CHECK(t.shape == shape_t({10, 100}) && t.data == a);
        auto q = r.load("b"); double e = 0; for (std::size_t i = 0; i < a.size(); ++i) e = std::max(e, double(std::fabs(q.data[i] - a[i])));
        CHECK(e < 0.03);
        CHECK(r.entry("c").codec == "f16");
        for (auto& [n, s] : r.sections()) CHECK(s.offset % modelfile::alignment == 0);
    }
    {   // flip one byte inside tensor data -> verify must report it
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        modelfile::reader r(path.string());
        auto off = r.sections().at("data").offset + r.entry("a").offset + 17;
        f.seekp(std::streamoff(off)); char c = 0x5a; f.write(&c, 1);
    }
    {
        modelfile::reader r(path.string());
        auto bad = r.verify();
        CHECK(bad.size() == 2);  // the data section and tensor "a"
    }   // unmap before deleting: Windows refuses to delete a mapped file
    std::filesystem::remove(path);
}

static const char* kTok = R"({"model": {"type": "BPE", "vocab": {"a": 0, "b": 1, "c": 2, "Ġ": 3, "ab": 4, "Ġab": 5, "!": 6},
  "merges": ["a b", "Ġ ab"]}, "added_tokens": [{"id": 7, "content": "<s>"}]})";

TEST(bpe_tokenizer_merges_and_roundtrip) {
    modelfile::bpe_tokenizer t(kTok);
    auto ids = t.encode("<s>ab abc!");
    CHECK((ids == std::vector<int>{7, 4, 5, 2, 6}));
    CHECK(t.decode(ids) == "<s>ab abc!");
}

TEST(blueprint_parse_build_and_errors) {
    const char* src = R"(
blueprint "mlp" opset 1
params { d: 4  scale: { a: 2 } }
input x : f32[n, d]
graph {
  h = x * scale.a + 1
  repeat i in 0..3 { h = tanh(h) }
  y = softmax(h)
}
output y : f32[n, d]
session one_shot)";
    auto bp = modelfile::parse_blueprint(src);
    CHECK(bp.name == "mlp" && bp.param("d") == 4 && bp.param("scale.a") == 2 && bp.session == "one_shot");
    modelfile::build_options bo; bo.dims["n"] = 3;
    auto g = modelfile::build_graph(bp, bo);
    auto p = graph::compile(g);
    graph::executor e(*p);
    std::vector<float> x(12, 0.5f); e.set_input("x", x); e.run();
    auto y = e.output("y");
    CHECK(y.shape == shape_t({3, 4}) && std::fabs(y.data[0] - 0.25f) < 1e-6f);
    // errors carry a line number
    bool threw = false;
    try { modelfile::build_graph(modelfile::parse_blueprint("blueprint \"e\"\ninput x : f32[2]\ngraph {\n y = nope(x)\n}\noutput y : f32[2]"), {}); }
    catch (const forge_exception& ex) { threw = std::string(ex.what()).find("blueprint:4") != std::string::npos; }
    CHECK(threw);
}

TEST(sampler_chain) {
    std::vector<float> l = {0.f, 5.f, 4.9f, -1.f};
    inference::sampler g; CHECK(g.sample(l) == 1);
    inference::sampler rp({.repetition_penalty = 2.f}); std::vector<int> h = {1}; CHECK(rp.sample(l, h) == 2);
    inference::sampler k1({.temperature = 1.f, .top_k = 1, .seed = 3}); for (int i = 0; i < 20; ++i) CHECK(k1.sample(l) == 1);
    inference::sampler t({.temperature = 1.f, .top_p = 0.9f, .seed = 7});
    int c[4] = {}; for (int i = 0; i < 2000; ++i) ++c[t.sample(l)];
    CHECK(c[0] == 0 && c[3] == 0 && c[1] > c[2] && c[2] > 700);
    inference::sampler mp({.temperature = 1.f, .min_p = 0.95f, .seed = 1}); for (int i = 0; i < 20; ++i) CHECK(mp.sample(l) == 1);
}

TEST(blueprint_composite_ops) {
    auto src = R"(blueprint "c"
op cube(x) { return x * x * x }
op my_gelu(x) {
  inner = 0.7978845608 * (x + 0.044715 * cube(x))
  return 0.5 * x * (1 + tanh(inner))
}
op silu(x) { return x * 0 }
op twice(a, b) { s = my_gelu(a) + b  return s * 2 }
input x : f32[8]
graph { y = my_gelu(x)  z = silu(x)  w = twice(x, x) }
output y : f32[8]
output z : f32[8]
output w : f32[8]
)";
    auto bp = modelfile::parse_blueprint(src); CHECK(bp.composites.size() == 4);
    auto p = graph::compile(modelfile::build_graph(bp, {})); graph::executor ex(*p);
    std::vector<float> x = {-3, -1, -0.5f, 0, 0.25f, 1, 2, 4}; ex.set_input("x", x); ex.run();
    auto y = ex.output_view("y"), z = ex.output_view("z"), w = ex.output_view("w");
    for (std::size_t i = 0; i < 8; ++i) {
        double gt = 0.5 * x[i] * (1 + std::tanh(0.7978845608 * (x[i] + 0.044715 * x[i] * x[i] * x[i])));
        CHECK(std::fabs(y.data[i] - gt) < 1e-5);
        CHECK(std::fabs(z.data[i] - x[i] / (1 + std::exp(-x[i]))) < 1e-5);   // spec op wins over the composite
        CHECK(std::fabs(w.data[i] - 2 * (gt + x[i])) < 1e-4);
    }
    CHECK_THROWS(modelfile::build_graph(modelfile::parse_blueprint("blueprint \"e\"\nop f(a, b) { return a }\ninput x : f32[2]\ngraph { y = f(x) }\noutput y : f32[2]"), {}));
    CHECK_THROWS(modelfile::build_graph(modelfile::parse_blueprint("blueprint \"e\"\nop f(a) { return f(a) }\ninput x : f32[2]\ngraph { y = f(x) }\noutput y : f32[2]"), {}));
    CHECK_THROWS(modelfile::parse_blueprint("blueprint \"e\"\nop f(a) { return a }\nop f(a) { return a }\ninput x : f32[2]\ngraph { y = f(x) }\noutput y : f32[2]"));
    CHECK_THROWS(modelfile::build_graph(modelfile::parse_blueprint("blueprint \"e\"\nop f(a) { b = a }\ninput x : f32[2]\ngraph { y = f(x) }\noutput y : f32[2]"), {}));
}

TEST(fpk_encryption_hook) {
    auto src = forge_test::make_tiny_model(tmp("enc_src.fpk")), dst = tmp("enc.fpk");
    auto key = modelfile::generate_model_key(); modelfile::encrypt_file(src, dst, key);
    {   // readers map the file; Windows cannot replace a mapped file, so they close before dst is rewritten
    modelfile::reader plain(src), locked(dst);
    CHECK(locked.locked()); CHECK(locked.verify().empty()); CHECK_THROWS(locked.load("lm_head"));
    CHECK(locked.manifest().str_or("format", "") == "fpk");                  // metadata stays readable
    modelfile::reader open(dst, key); CHECK(open.encrypted()); CHECK(open.verify().empty());
    for (auto& [n, e] : plain.tensors()) CHECK(open.load(n).data == plain.load(n).data);
    { auto raw = open.raw_section_bytes("data"), pd = plain.section_bytes("data"); CHECK(raw.size() == pd.size() && std::memcmp(raw.data(), pd.data(), pd.size()) != 0); }
    auto bad = key; bad[0] ^= 1; CHECK_THROWS(modelfile::reader(dst, bad));
    CHECK_THROWS(modelfile::encrypt_file(dst, tmp("enc2.fpk"), key));
    // tamper with one ciphertext byte: decryption must fail
    { std::fstream f(dst, std::ios::in | std::ios::out | std::ios::binary); auto off = std::streamoff(locked.sections().at("data").offset + 5);
      f.seekg(off); char c; f.get(c); f.seekp(off); f.put(char(c ^ 0x40)); }
    CHECK_THROWS(modelfile::reader(dst, key));
    }
    // sign + verify an encrypted file without the key, then load it with the key from the environment
    modelfile::encrypt_file(src, dst, key);
    auto kp = modelfile::generate_keypair(); modelfile::sign_file(dst, dst, kp);
    { modelfile::reader r(dst); auto rep = modelfile::verify_signature(r); CHECK(rep.digest_ok && !rep.valid_keys.empty()); }
    forge_test::set_env("FORGE_MODEL_KEY", modelfile::detail::hex(key.data(), 32).c_str());
    { inference::session s(dst); auto l = s.forward(std::vector<int>{1, 2}); inference::session p(src); CHECK(l == p.forward(std::vector<int>{1, 2})); }
    forge_test::set_env("FORGE_MODEL_KEY", nullptr);
}
