// docs/inference.md example: open a .fpk, tokenize, generate greedily, detokenize.
// Usage: doc_example_inference_generate model.fpk "prompt"
#include <forge/inference/inference.hpp>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s model.fpk [prompt]\n", argv[0]); return 2; }
    using namespace forge::inference;
    session_options opt; opt.context = 256;
    forge::inference::session s(argv[1], opt);   // qualified: FreeBSD <sys/user.h> declares a global struct session
    const auto* tok = s.tokenizer();
    if (!tok) { std::fprintf(stderr, "model has no tokenizer\n"); return 1; }
    std::vector<int> prompt = tok->encode(argc > 2 ? argv[2] : "Hello");
    sampler_config sc; sc.temperature = 0.f;                   // greedy
    sampler smp(sc);
    std::vector<int> out = s.generate(prompt, 16, smp);
    std::printf("%zu tokens: %s\n", out.size(), tok->decode(out).c_str());
    return out.empty() ? 1 : 0;
}
