/* C11 consumer of forge.h: proves the ABI is usable from plain C. Usage: capi_smoke model.fpk */
#include <forge.h>
#include <stdio.h>
static int on_tok(int32_t t, const char* txt, void* u) { (void)txt; ++*(int*)u; return 0; }
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (forge_abi_version() != FORGE_ABI_VERSION) return 3;
    if (forge_open("/nonexistent.fpk", 0, 0) != NULL || forge_last_error()[0] == 0) { puts("error path broken"); return 4; }
    forge_model* m = forge_open(argv[1], 64, 1);
    if (!m) { printf("open failed: %s\n", forge_last_error()); return 1; }
    int32_t ids[64]; int64_t n = forge_tokenize(m, "the theory", 1, ids, 64);
    char buf[128]; forge_detokenize(m, ids + 1, n - 1, buf, sizeof buf);
    forge_sampling s = {0.f, 0, 1.f, 0.f, 1.f, 0};
    int32_t out[8]; int calls = 0;
    int64_t g = forge_generate(m, ids, n, 8, &s, out, 8, on_tok, &calls);
    printf("tokens %lld, roundtrip \"%s\", generated %lld (callbacks %d), vocab %lld\n", (long long)n, buf, (long long)g, calls, (long long)forge_vocab_size(m));
    forge_close(m);
    return (g == 8 && calls == 8 && n > 1) ? 0 : 1;
}
