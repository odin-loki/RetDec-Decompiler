/* docs/capi.md example: the C ABI end to end. Usage: doc_example_capi_generate model.fpk */
#include <forge.h>
#include <stdio.h>

static int on_token(int32_t token, const char* text, void* user) {
    (void)token; (void)text; ++*(int*)user;
    return 0;                                   /* non-zero stops generation */
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (forge_abi_version() < 3) return 1;
    forge_model* m = forge_open(argv[1], 256, 0);
    if (!m) { fprintf(stderr, "open failed: %s\n", forge_last_error()); return 1; }
    int32_t prompt[64], out[16];
    int64_t n = forge_tokenize(m, "Hello", 1, prompt, 64);
    forge_sampling s = {0.f, 0, 1.f, 0.f, 1.f, 42};   /* temperature, top_k, top_p, min_p, rep_pen, seed */
    int seen = 0;
    int64_t got = forge_generate(m, prompt, n, 16, &s, out, 16, on_token, &seen);
    char text[256];
    forge_detokenize(m, out, got, text, sizeof text);
    printf("forge %s abi %u: %lld tokens (%d callbacks), position %lld\n", forge_version(), forge_abi_version(),
           (long long)got, seen, (long long)forge_position(m));
    forge_close(m);
    return got > 0 && seen == got ? 0 : 1;
}
