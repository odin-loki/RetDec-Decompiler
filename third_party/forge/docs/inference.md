# inference: models, sessions and generation

Umbrella header: `<forge/inference/inference.hpp>` (sampler, session, speculative decoding, grammar, batch scheduler). `json_schema.hpp`, `diffusion.hpp`, `streaming.hpp` and `plugin.hpp` are included separately. Link `forge_inference`, `forge_verify` and `forge_simd_mv`. Namespace `forge::inference`.

## Concepts
- A **`model`** is an opened `.fpk`. It holds the memory-mapped file, blueprint, manifest, tokenizer, compiled plans, LoRA adapters and the device choice. It is shared (`std::shared_ptr<model>`) by any number of sessions. Graph runs on one model are serialised by a mutex.
- A **`session`** holds one sequence's state: the paged KV store and the position. The blueprint's session kind decides how it is used: `generative_sequence` (language models: `generate`, `forward`) or a one-shot graph (`infer`, e.g. ONNX imports).
- Other session kinds live in their own headers and do not need a `.fpk`: `diffusion::session`, `streaming::session` and `plugin::session` (kind `custom`).
- A **`sampler`** turns logits into a token with a fixed chain: repetition / presence / frequency penalties → optional `filter` (grammar) → temperature → top-k → top-p → min-p → typical-p → draw. Mirostat v2 replaces the truncation steps when `mirostat_tau > 0`. Temperature 0 is greedy (argmax after penalties and filter).
- **Weights** stay in the mapped file. f32 tensors are used in place; quantised tensors are read in place by quantised matmul. `FORGE_DECODE_WEIGHTS=1` decodes every weight to f32 up front instead.

Architectures come from the import templates (Hugging Face: Llama/Mistral/Qwen2, Qwen3, Qwen3.5 text, Mixtral, DeepSeek-V2/V3; GGUF has its own list): see [modelfile.md](modelfile.md). Qwen3 and Qwen3.5 are covered by the `golden_tiny_qwen3` and `golden_tiny_qwen3_5` ctests (the latter skips without torch and transformers ≥ 5).

## API reference

### `model` (`session.hpp`)

| Member | Meaning |
|---|---|
| `model::open(path, verify = false)` | Returns `std::shared_ptr<model>`. `verify` checks BLAKE3 hashes, and the signature if the file has one |
| `set_device(name)` | Picks the execution device (`cpu`, `vulkan`, `cuda:0`, ...; see [configuration.md](configuration.md)). An unavailable device falls back to `cpu` with a warning. Returns the device used and clears cached plans. `FORGE_DEVICE` is applied on open |
| `device()`, `blueprint()`, `manifest()`, `file()` | Accessors |
| `tokenizer()` | `const modelfile::bpe_tokenizer*`, `nullptr` if the file has none |
| `vocab()`, `max_context()` | `max_context` is manifest `max_position_embeddings`, 2048 if absent |
| `add_adapter(name, {weight → {a, b}}, alpha)` | LoRA: `W' = W + (alpha / r) · B·A` (PEFT layout, A `[r, in]`, B `[out, r]`). Only targeted matrices are copied; names are immutable once added |
| `add_adapter_file(name, path)` | Loads `<weight>.lora_a` / `<weight>.lora_b` tensors from an `.fpk`; alpha from manifest `lora_alpha` (default 1) |
| `has_adapter(name)` | `""` (base) is always present |

Strict loading: `FORGE_REQUIRE_SIGNED=1` and/or `FORGE_TRUSTED_KEYS=<hex>[,<hex>...]` make `model` refuse files without a valid (trusted) signature, whatever `verify` says.

### `session_options`

| Field | Default | Meaning |
|---|---|---|
| `context` | 0 | KV capacity in tokens; 0 = `model::max_context()` |
| `verify` | false | Check hashes when the session opens the model by path |
| `page_rows` | 256 | KV page size in rows; 0 = reserve the whole context up front |
| `kv_codec` | `""` | `f32`, `int8`, `fp8`, `tbq4`, `tbq3`, `tbq4p`, `tbq3p`, `k8v4` (int8 keys, tbq4 values). Empty = `$FORGE_KV_CACHE_TYPE`, then `$FORGE_KV_CODEC`, then f32 |
| `kv_offload` | -1 | 1 = keep KV on the host and drop device copies after each attention; -1 = `$FORGE_KV_OFFLOAD` |

### `session`

| Member | Meaning |
|---|---|
| `session(path, opt)` / `session(shared_ptr<model>, opt)` | Opening by path creates a private model |
| `generate(prompt, max_new, sampler&, on_token = {}, generate_stats* = nullptr, stop_at_eos = true)` | Prefill then decode. Continues from the current position (it does not reset). Returns the new ids, including EOS if sampled. `on_token(int)` returning `false` stops. Also stops at `max_new` or a full context |
| `forward(ids)` | Runs `ids` from the current position; returns last-row logits |
| `forward_all(ids)` | Same; returns `[ids.size(), vocab]` logits |
| `position()`, `context()`, `reset()`, `rewind(pos)` | `rewind` only moves backwards |
| `copy_prefix(other, n)` | Copies the first `n` tokens of another session's KV (same model and KV codec) |
| `save_prefix(n = -1)` / `load_prefix(blob)` | Serialise / restore KV state of a prefix (`FORGEKV1` blob). Restoring gives the same logits as `copy_prefix` |
| `set_adapter(name)` | Select a LoRA adapter (`""` = base). Clears the KV cache when it changes |
| `infer({name → data}, seq = 1)` | One-shot graphs: binds every declared input, runs once, returns every output |
| `tokenizer()` | `encode`, `decode`, `decode_one`, `bos()`, `eos()` |

`generate_stats` has `prompt_tokens`, `new_tokens`, `prefill_s`, `decode_s`.

### `sampler_config` (`sampler.hpp`)

| Field | Default (= off) | Notes |
|---|---|---|
| `temperature` | 0 | 0 = greedy |
| `top_k` | 0 | |
| `top_p`, `min_p`, `typical_p` | 1, 0, 1 | |
| `mirostat_tau`, `mirostat_eta` | 0, 0.1 | Mirostat v2; tau in bits |
| `repetition_penalty` | 1 | Divides positive / multiplies negative logits of seen tokens |
| `presence_penalty`, `frequency_penalty` | 0, 0 | OpenAI-style, subtracted from logits |
| `repetition_window` | 64 | History length all three penalties look at |
| `seed` | 0 | `mt19937_64` seed |

`sampler` also has `probs(logits, history)` (full distribution after the chain), `draw(p)` and a public `filter` hook (`std::function<void(std::span<float>)>`).

### Speculative decoding (`speculative.hpp`)
- `speculative_generate(target, draft, prompt, max_new, sampler&, k = 4, on_token = {}, stats = nullptr, spec_planner* = nullptr)`: a draft model proposes tokens, the target scores the current token and all drafts in one pass. Greedy (and greedy-with-penalties) drafts are verified by sample-and-compare, sampled drafts by modified rejection sampling; either way the output keeps the target's distribution, and greedy output equals target greedy output. The draft keeps its own KV and only catches up on kept tokens it has not seen. Vocab sizes must match. Resets both sessions first.
- `lookup_generate(target, prompt, max_new, sampler&, k = 4, ngram = 3, on_token = {}, stats = nullptr, spec_planner* = nullptr, ngram_min = 1)`: prompt-lookup (n-gram) speculation from the prompt and output (`ngram_index`: hashed n-grams, O(ngram) per lookup; a match that runs into the tail repeats with its period). Verified by sample-and-compare, so with a fixed seed the text is the same as plain decoding whenever the pass logits equal the single-token ones (always on the CPU).
- `k = 0` (the CLI default): `spec_planner` (`spec_planner.hpp`) chooses the drafts per pass to maximise (1 + sum of acceptance chances) / (pass time + draft time), from moving-average pass times by row count (first pass of each size skipped: plan compile) and per-position acceptance counts (prior 1/2, halved at 192 trials). It picks 0 drafts when no draft count beats a plain pass, and forces a 1-draft probe after 16 draft-less passes in a row. `kmax` = 8.
- Recurrent models (Qwen3.5 DeltaNet / conv state): `session::forward_spec(ids, keep)` runs the verify pass and sets the recurrent state aside after the first `keep` rows inside that pass (the executor splits the state op there; on CUDA/HIP a device-to-device copy, no host transfer). `spec_rewind(pos)` restores it and returns how many accepted drafts must be fed again; the generators put them at the head of the next pass (replay rows) instead of running an extra pass. `make_checkpoint` / `rewind(pos, ck, since)` still exist and are used only for recurrent *draft* models.
- `speculative_stats`: `rounds` (passes with drafts), `passes`, `drafted`, `accepted`, `replayed`, `probes`, `pos_tried` / `pos_won` (by draft position), `new_tokens`, `seconds`, `prefill_seconds`, `acceptance()`, `decode_tps()`, `tokens_per_pass()`.
- Sampler: candidates are the tokens within 40 nats of the best (a scan, then a sort of those or `nth_element` for top-k) instead of a sort of the whole vocabulary; the result is the same (the terms left out are below half an ulp of the sum). `sampler::allow` (set by the CLI, C API, script and batch scheduler from `grammar_constraint::allows`) tests the likeliest candidates against the grammar first and falls back to the whole-vocabulary `filter` only after 256 tests.

### Grammars and JSON Schema
- `grammar(src, root = "root")` (`grammar.hpp`) parses GBNF in the llama.cpp dialect: rules, literals with `\n \t \r \\ \" \xHH`, byte classes with ranges and `^`, grouping, `|`, `* + ?`, `#` comments. Matching is byte-level. Left recursion is rejected.
- `grammar_constraint(shared_ptr<const grammar>, tokenizer, vocab, eos)` masks tokens that cannot continue (`apply(logits)`), advances on `accept(token)`, and reports `can_end()` / `finished()`. EOS is allowed only when the grammar can end.
- `json_schema_to_gbnf(json::value)` (`json_schema.hpp`) converts a JSON Schema subset to GBNF producing compact JSON (no whitespace): `type` (incl. lists), `properties` (declaration order; not in `required` = optional), `items`, `minItems`/`maxItems` ≤ 64, `enum`, `const`, `anyOf`/`oneOf`, `$ref` to `#/$defs/*` or `#/definitions/*`. An empty schema accepts any JSON value. Other keywords are ignored or rejected with `invalid_argument`.

### Continuous batching (`batch.hpp`)
`batch_scheduler(shared_ptr<model>, batch_options)`. Each `step()` admits waiting requests up to `max_active`, prefills newcomers in chunks of `prefill_chunk` tokens and advances every active sequence by one token.

| `batch_options` field | Default | Meaning |
|---|---|---|
| `max_active` | 8 | Sequences decoding at once |
| `prefill_chunk` | 256 | Prompt tokens per step per sequence |
| `prefix_cache` | 4 | Finished sequences kept in memory for prefix reuse (LRU); 0 = off |
| `session` | `{}` | `session_options` for every sequence |
| `budget` | `nullptr` | `mem::budget`: KV bytes are reported (tag `kv`), the prefix cache registers as evictable, and new requests wait while the budget is over its soft watermark (one request always runs). See [memory.md](memory.md) |
| `spill` | `nullptr` | `mem::spill_store`: evicted prefixes are written to disk (`save_prefix`) and read back on a hit instead of recomputed |
| `spill_entries` | 64 | Maximum prefixes kept in the spill store |

`request` fields: `prompt`, `max_new` (64), `sampling`, `grammar` (`shared_ptr<const grammar>`), `stop_tokens` (in addition to EOS), `priority` (higher first, FIFO within a priority), `on_token(int)` (return `false` to cancel), `on_done(tokens, finish_reason)`. `finish_reason` is `stop`, `length`, `grammar` (sampled token rejected), `cancelled`, or `error: <message>`.

Methods: `submit(request)` → id, `cancel(id)`, `step()`, `run_until_idle()`, `start()` / `stop()` (background worker), `request_kv_eviction(keep)` (safe from any thread), `idle()`, `waiting()`, `active_count()`, `spilled()`, `evictions()`, `stats()`. `batch_stats`: `steps`, `completed`, `prefix_hits`, `prefix_tokens_reused`, `peak_active`, `peak_kv_bytes`, `deferred`, `prefix_spills`, `prefix_reloads`.

A new prompt reuses the longest prefix it shares with a cached (in memory or spilled) sequence, always leaving at least one prompt token to compute.

### Other session kinds
- `diffusion.hpp` (`forge::inference::diffusion`): `noise_scheduler::make(T, beta_schedule, b0, b1)` (linear, scaled_linear, cosine), `options` (`steps`, `kind` = ddim / ddpm / euler_flow, `pred` = epsilon / v / sample, `guidance` (classifier-free, 1 = off), `eta`, `seed`, `cache_at`, `start_step`), `session(scheduler, denoiser).run(x, opt)`. The denoiser is any callable `(x, t, cond) → prediction`. Noise is Philox, so a seed gives the same trajectory on every backend. `cache()` holds latents for `cache_at` steps; resume with `start_step`.
- `streaming.hpp` (`forge::inference::streaming`): lock-free SPSC `ring<T>`, and `session(options, frame_fn)` with `feed(p, n)` (producer) and `pump()` (consumer). `options`: `window` 400, `hop` 160, `max_lag_frames` 8, `deadline`, `capacity` 65536. When more than `max_lag_frames` frames are pending, the oldest are dropped. `stat()` counts frames, drops, overruns and rejected samples.
- `plugin.hpp` (`forge::inference::plugin`): register a non-neural engine as a `service` (`run(tensors) → tensors`) under a name with `registry::global().add` or a static `registrar`; `plugin::session(kind, config).infer(in)`.

## Examples
`docs/examples/inference_generate.cpp` (ctest `doc_example_inference_generate`, run on `golden/tiny_f32.fpk`):

```cpp
using namespace forge::inference;
session_options opt; opt.context = 256;
forge::inference::session s(argv[1], opt);   // qualified: FreeBSD <sys/user.h> has a global `struct session`
std::vector<int> prompt = s.tokenizer()->encode("Hello");
sampler_config sc; sc.temperature = 0.f;     // greedy
sampler smp(sc);
std::vector<int> out = s.generate(prompt, 16, smp);
std::printf("%s\n", s.tokenizer()->decode(out).c_str());
```

Batching with a grammar (not a compiled example):

```cpp
auto m = model::open("model.fpk");
batch_scheduler bs(m, {.max_active = 4});
request r; r.prompt = m->tokenizer()->encode("Answer: "); r.max_new = 32;
r.grammar = std::make_shared<grammar>(json_schema_to_gbnf(forge::json::parse(R"({"enum":["yes","no"]})")));
r.on_done = [](const std::vector<int>& toks, const std::string& why) { /* ... */ };
bs.submit(std::move(r));
bs.run_until_idle();
```

`examples/integrations/` shows the other session kinds: `camera_detector.cpp` (diffusion), `neuralvfx_stream.cpp` (streaming), `cypha_classifier.cpp` (plugin), `seatbelt_one_shot.cpp` (one-shot `infer`), `fuse_runtime.cpp` (C ABI).

## Notes and limits

- CPU attention (`graph/attn_cpu.hpp`, kernels in `simd/seq.hpp`) cuts positions into absolute 256-position chunks; a work item (KV head, query-token block, chunk) serves every query head of the GQA group, so each K/V row (f32 or int8) is converted once per item. Chunk partials (max, sum of exponentials, weighted V) merge in chunk order (flash-decoding), so a token gets identical bits alone or inside a prefill block, and the scalar, AVX2 and AVX-512 tables agree bit for bit (own exp, fixed reduction trees, no contraction). It is the default with an int8 KV cache (Qwen3.5 9B, 1K tokens, 32 threads: prefill 3.2 → 6.4 tok/s, decode 0.6 → 2.2 on a loaded machine); with f32 KV the previous kernel stays the default until the chunked one is shown faster there (`FORGE_CPU_ATTN=new` opts in, `old` restores the previous kernel everywhere). The int16-query path and a head-major int8 KV layout from the janas study are not implemented.
- CPU delta_rule works per (value head, 32 value columns): one visit to the state slice per token, all tokens of a call walked with the slice in L1. The state stays f32 `[Hv, dk, dv]` (shared with the GPU kernels); `FORGE_CPU_DELTA=old` restores the previous kernel.
- Tests: `tests/test_inference.cpp` covers paged KV, `forward_all`, rewind / `copy_prefix`, speculative and lookup decoding (planner, n-gram index, seed-exact lookup, recurrent rollback), penalties, grammars, JSON Schema, batching (priority, cancel, threaded), typical / Mirostat and LoRA. Prefix spill is covered in `tests/test_memory.cpp`.
- The golden test models have random weights, so their text is meaningless. Tests compare numerics against references (`verify/golden/*.py`), not text quality.
- Recurrent state (e.g. Qwen3.5's Gated DeltaNet layers) cannot be cut: plain `rewind`, `copy_prefix` / `save_prefix` with fewer tokens than held raise `unsupported` (speculative decoding uses `forward_spec` / `spec_rewind` or a checkpoint instead). In `batch_scheduler` a partial prefix-cache hit on such a model raises inside `step()`, which ends the active requests with `error: ...` (read from the code; no test covers it).
- Runs on one `model` are serialised. Many sessions on one model do not run in parallel; `batch_scheduler` interleaves them step by step.
- `FORGE_GPU_REPLAY` enables experimental whole-run decode replay on CUDA/HIP (opt-in, no measured gain yet). See [configuration.md](configuration.md) and [gpu.md](gpu.md).
- The `forge run` CLI exposes sampling, `--grammar` and `--draft`; see [cli.md](cli.md). The C ABI is in [capi.md](capi.md).
