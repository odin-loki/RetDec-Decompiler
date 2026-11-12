# C ABI (`forge.h`)

## Concepts
`inference/include/forge.h` is a stable C interface to the inference engine. The Python, Kotlin and Swift bindings ([bindings.md](bindings.md)) are built on it. The full declarations are in [reference.md](reference.md).

- Handles are opaque. A `forge_model*` owns one model **and** one session (KV state and position). Handles do not share weights; open the file twice for two independent sequences.
- A handle is not thread-safe. Use one per thread, or lock around it.
- The ABI is versioned (`FORGE_ABI_VERSION 3`). Each version only adds functions; check `forge_abi_version()` at run time.
- Errors: functions return `NULL` or `-1`, and `forge_last_error()` gives the message. It is thread-local and valid until the next call on that thread.
- `FORGE_API` exports the functions: `__declspec(dllexport/dllimport)` on Windows (define `FORGE_C_STATIC` to link a static copy), default visibility elsewhere.

CMake target `forge_c` builds the shared library from `inference/capi/forge_c.cpp`: `libforge.so` (soname `libforge.so.1`), `libforge.dylib`, or `forge.dll` on Windows. Device selection, KV codec and other settings come from the environment (`FORGE_DEVICE`, `FORGE_KV_CACHE_TYPE`, ...; see [configuration.md](configuration.md)).

## API reference

| Function | ABI | Purpose |
|---|---|---|
| `forge_abi_version()`, `forge_version()`, `forge_last_error()` | 1 | Version (`forge_version` returns `"0.1.0"`) and last error |
| `forge_open(path, context, verify_hashes)` | 1 | Open a `.fpk`. `context` 0 = the model's maximum. Returns `NULL` on error |
| `forge_close(m)` | 1 | Free the handle |
| `forge_reset(m)` | 1 | Clear the KV state (position 0) |
| `forge_vocab_size(m)` | 1 | Vocabulary size |
| `forge_tokenize(m, text, add_bos, out, cap)` | 1 | Returns the full token count (writes at most `cap`), or -1. Call with `cap` 0 to size the buffer. `add_bos`: 1 prepend BOS, 0 not, -1 the model's default (the manifest's `add_bos`; Qwen-family models never add BOS) |
| `forge_detokenize(m, ids, n, out, cap)` | 1 | Writes NUL-terminated text (truncated to `cap - 1` bytes); returns the bytes needed without the NUL, or -1. `out` may be `NULL` |
| `forge_forward(m, ids, n, logits, cap)` | 1 | Runs `ids` from the current position; writes up to `cap` floats of the last row's logits. Returns 0 or -1 |
| `forge_generate(m, prompt, n, max_new, &sampling, out, cap, cb, user)` | 1 | Prefill and decode. Returns the number of tokens generated (may exceed `cap`; only `cap` are written), or -1 |
| `forge_position(m)` | 2 | Tokens in the KV state |
| `forge_rewind(m, pos)` | 2 | Roll the KV state back to `pos` tokens. 0 or -1 |
| `forge_forward_all(m, ids, n, logits, cap)` | 2 | Writes `n × vocab` logits (up to `cap`) |
| `forge_generate_ex(..., const forge_generate_options*, ...)` | 2 | Adds a GBNF `grammar`, a speculative `draft` model and `spec_k` (≤ 0 → 4) |
| `forge_generate_ex2(..., const forge_generate_options*, const forge_sampling_ext*, ...)` | 3 | Adds `typical_p`, `mirostat_tau`, `mirostat_eta` |

### Structs

| Struct | Fields |
|---|---|
| `forge_sampling` | `temperature` (0 = greedy), `top_k` (0 = off), `top_p` (1 = off; ≤ 0 treated as 1), `min_p` (0 = off), `repetition_penalty` (1 = off; ≤ 0 treated as 1), `seed` |
| `forge_generate_options` | `grammar` (GBNF text or `NULL`), `draft` (`forge_model*` with the same vocab, or `NULL`), `spec_k` |
| `forge_sampling_ext` | `size` (set to `sizeof(forge_sampling_ext)`; fields past `size` are ignored), `typical_p` (0 or 1 = off), `mirostat_tau` (0 = off), `mirostat_eta` (0 = 0.1) |

Either options pointer may be `NULL`. Presence and frequency penalties are not exposed in the C ABI.

### Generation semantics
- `forge_generate*` **continues from the current KV position**: the prompt is appended to what the handle already holds. Call `forge_reset` first for a fresh sequence. With a `draft` model, both handles are reset first.
- Generation stops at `max_new` tokens, at EOS (EOS is included in the output), when the context is full, when the callback returns non-zero, or when the grammar is complete or rejects a token.
- The callback is `int cb(int32_t token, const char* text, void* user)`. `text` is that token's bytes, which may be part of a UTF-8 sequence. Pass `NULL` for no callback.
- After generation, `forge_position` is the prompt length plus the generated count minus 1: the last sampled token has not been fed through the model yet.

## Example
`docs/examples/capi_generate.c` (ctest `doc_example_capi_generate`, run on `golden/tiny_f32.fpk`). `tests/capi_smoke.c` (ctest `capi_smoke`) exercises more of the API.

```c
if (forge_abi_version() < 3) return 1;
forge_model* m = forge_open(path, 256, 0);
if (!m) { fprintf(stderr, "%s\n", forge_last_error()); return 1; }
int32_t prompt[64], out[16];
int64_t n = forge_tokenize(m, "Hello", 1, prompt, 64);
forge_sampling s = {0.f, 0, 1.f, 0.f, 1.f, 42};   /* temperature, top_k, top_p, min_p, rep_pen, seed */
int64_t got = forge_generate(m, prompt, n, 16, &s, out, 16, on_token, &user);
forge_close(m);
```

Grammar-constrained, with typical sampling:

```c
forge_generate_options o = {"root ::= \"yes\" | \"no\"", NULL, 0};
forge_sampling_ext x = {sizeof x, 0.9f, 0.f, 0.f};
forge_reset(m);
int64_t got = forge_generate_ex2(m, prompt, n, 8, &s, &o, &x, out, 16, NULL, NULL);
```

## Platform notes and limits
- Cross builds (`CMAKE_CROSSCOMPILING`) do not build `capi_smoke` or this example; the ctests also need `FORGE_BUILD_APPS` (they use the golden model).
- C consumers link with the C++ driver (`LINKER_LANGUAGE CXX`) because the library needs the C++ runtime.
- With `-DFORGE_JNI=ON` (set in the `android-arm64` preset) the build also produces `libforge_jni`, the JNI shim used by the Kotlin binding.
- `tools/build_xcframework.sh` packages `forge_c` for iOS, the iOS simulator and macOS (not run here; see [bindings.md](bindings.md)).
