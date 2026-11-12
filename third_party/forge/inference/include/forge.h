/* forge.h — stable C ABI for embedding FORGE (FUSE, Cypha, anything). ABI-versioned; all handles opaque. */
#ifndef FORGE_H
#define FORGE_H
#include <stddef.h>
#include <stdint.h>
/* FORGE_API marks the exported functions. Building libforge defines FORGE_C_BUILD; link a static copy with
   FORGE_C_STATIC. On Windows the functions are dllexport/dllimport (MSVC exports nothing by default). */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(FORGE_C_BUILD)
#    define FORGE_API __declspec(dllexport)
#  elif defined(FORGE_C_STATIC)
#    define FORGE_API
#  else
#    define FORGE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define FORGE_API __attribute__((visibility("default")))
#else
#  define FORGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_ABI_VERSION 3   /* 2 = v1 + position/rewind/forward_all/generate_ex; 3 = + generate_ex2 / forge_sampling_ext (additive) */

typedef struct forge_model forge_model;

typedef struct forge_sampling {
    float temperature;        /* 0 = greedy */
    int32_t top_k;            /* 0 = off */
    float top_p;              /* 1 = off */
    float min_p;              /* 0 = off */
    float repetition_penalty; /* 1 = off */
    uint64_t seed;
} forge_sampling;

/* Returns non-zero to stop generation. */
typedef int (*forge_token_cb)(int32_t token, const char* text, void* user);

FORGE_API uint32_t forge_abi_version(void);
FORGE_API const char* forge_version(void);
FORGE_API const char* forge_last_error(void);          /* thread-local; valid until the next call on this thread */

FORGE_API forge_model* forge_open(const char* fpk_path, int64_t context, int verify_hashes);
FORGE_API void forge_close(forge_model* m);
FORGE_API void forge_reset(forge_model* m);             /* clear KV state */
FORGE_API int64_t forge_vocab_size(const forge_model* m);

/* Tokenizer. Return token count, or -1 on error; writes at most cap tokens. */
FORGE_API int64_t forge_tokenize(const forge_model* m, const char* text, int add_bos, int32_t* out, int64_t cap);
/* Writes NUL-terminated text, returns bytes needed (excluding NUL) or -1. */
FORGE_API int64_t forge_detokenize(const forge_model* m, const int32_t* ids, int64_t n, char* out, int64_t cap);

/* Runs tokens continuing from the current position; writes last-position logits (vocab floats). */
FORGE_API int forge_forward(forge_model* m, const int32_t* ids, int64_t n, float* logits, int64_t cap);

/* Prefill + decode. Returns number of generated tokens written to out (or -1). */
FORGE_API int64_t forge_generate(forge_model* m, const int32_t* prompt, int64_t n_prompt, int32_t max_new,
                       const forge_sampling* s, int32_t* out, int64_t cap, forge_token_cb cb, void* user);

/* ---- ABI 2 ---- */
FORGE_API int64_t forge_position(const forge_model* m);
FORGE_API int forge_rewind(forge_model* m, int64_t position);  /* roll KV state back to `position` tokens */
/* Logits for every position of ids: writes n * vocab floats. */
FORGE_API int forge_forward_all(forge_model* m, const int32_t* ids, int64_t n, float* logits, int64_t cap);

typedef struct forge_generate_options {
    const char* grammar;   /* GBNF text constraining the output, or NULL */
    forge_model* draft;    /* speculative-decoding draft model (same vocab), or NULL */
    int32_t spec_k;        /* draft tokens per round (default 4) */
} forge_generate_options;
FORGE_API int64_t forge_generate_ex(forge_model* m, const int32_t* prompt, int64_t n_prompt, int32_t max_new,
                          const forge_sampling* s, const forge_generate_options* o,
                          int32_t* out, int64_t cap, forge_token_cb cb, void* user);

/* ABI 3: extra samplers. Size-prefixed so fields can be appended later: set size = sizeof(forge_sampling_ext). */
typedef struct forge_sampling_ext {
    uint32_t size;
    float typical_p;          /* locally typical sampling; 1 (or 0) = off */
    float mirostat_tau;       /* Mirostat v2 target surprise (bits); 0 = off */
    float mirostat_eta;       /* Mirostat learning rate; 0 = default 0.1 */
} forge_sampling_ext;
FORGE_API int64_t forge_generate_ex2(forge_model* m, const int32_t* prompt, int64_t n_prompt, int32_t max_new,
                           const forge_sampling* s, const forge_generate_options* o, const forge_sampling_ext* ext,
                           int32_t* out, int64_t cap, forge_token_cb cb, void* user);

#ifdef __cplusplus
}
#endif
#endif
