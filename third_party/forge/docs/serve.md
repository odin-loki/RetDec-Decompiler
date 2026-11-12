# serve: the forge-serve HTTP server

## Concepts
`forge-serve` (`apps/forge_serve.cpp`) serves one generative model over HTTP with an OpenAI-style API. All requests go through one continuous-batching scheduler (`inference::batch_scheduler`, see [inference.md](inference.md)), which reuses KV rows for prompts that share a prefix with a recent request.

- The model argument may be an `.fpk`, a Hugging Face directory or a `.gguf`. The last two are imported into `<tmp>/<name>.serve.fpk` at startup, using `--codec` (default `f32`).
- Models whose session kind is not `generative_sequence` are refused.
- One thread per connection; every response closes the connection (`Connection: close`).
- `forge-serve proxy ...` runs the same binary as a load-balancing proxy: see [loadbalancer.md](loadbalancer.md).

## API reference

### Command line

```
forge-serve <model.fpk|hf_dir|model.gguf> [--host 127.0.0.1] [--port 8080] [--max-active 8] [--ctx 0]
            [--codec f32] [--watch] [--no-pin] [--numa-node N] [--mem-limit 6G|75%|off] [--max-queue 0] [--kv-spill]
            [--policy FILE]
forge-serve proxy --backend HOST:PORT [...] [--policy FILE]   # see loadbalancer.md
```

| Flag | Default | Effect |
|---|---|---|
| `--host`, `--port` | `127.0.0.1`, `8080` | IPv4 listen address. `--port 0` picks a free port (printed on startup) |
| `--max-active` | 8 | Sequences decoded together (`batch_options::max_active`) |
| `--ctx` | 0 | KV context per request; 0 = the model's maximum |
| `--codec` | `f32` | Weight codec for the import of a HF directory or `.gguf`; ignored for `.fpk` |
| `--mem-limit` | `FORGE_MEM_LIMIT`, else 85% of RAM or the cgroup limit | Host memory budget (`6G`, `512M`, `75%`; `0`/`off` disables). Under it the scheduler reports live KV bytes, evicts cached prefixes, and keeps new requests queued while the budget is over its soft watermark, so a tight limit runs requests one at a time instead of failing. See [memory.md](memory.md) |
| `--max-queue N` | 0 (no cap) | Once N requests are waiting, new ones get `503` with `Retry-After: 1` |
| `--kv-spill` | off | Cached prefixes evicted from memory are written to the spill directory (`FORGE_MEM_SPILL_DIR`, capped by `FORGE_MEM_SPILL_MAX`) and read back when a new prompt shares them, instead of being recomputed. Up to 64 prefixes |
| `--watch` | off | Watches the model path. On change: waits 0.5 s, sends systemd `RELOADING=1`, closes the listener and re-execs itself with the same arguments. POSIX only; on Windows it logs that a restart is needed |
| `--no-pin` | pinning on | On hybrid CPUs forge-serve pins itself (and the threads it starts) to performance cores. This turns that off |
| `--numa-node N` | none | Binds to NUMA node N before loading, so weights and KV are first-touched there |
| `--policy FILE` | `load_policy()` search | FORGE Security Standard policy; see Security policy below |

Fixed behaviour (no flag):
- The prefix cache keeps 4 finished sequences; prompts are prefilled in chunks of 256 tokens.
- OS memory-pressure events evict the prefix cache (moderate: keep 1, critical: keep 0) and log a warning to the system log.
- After the socket listens, forge-serve calls `service::ready()` (systemd `READY=1`; a no-op without `NOTIFY_SOCKET`).
- `FORGE_HUGEPAGES=1` advises huge pages on weight mappings ≥ 2 MiB (see [configuration.md](configuration.md)).
- `FORGE_DEVICE` picks the device, as for every model.

### Security policy
With a policy (`--policy FILE`, `$FORGE_SECURITY_POLICY`, `./forge-security.json`, the config dir or `$FORGE_SECURITY_PROFILE`; see [security-api.md](security-api.md)); with none of them nothing below happens:

- The policy is active before the model is loaded: the model path must be readable under `fs.read`, and `integrity.models` decides (`required`: a valid signature, by one of `integrity.trusted_keys` if set, else exit 1 with an integrity error; `optional`: signed files are verified, unsigned ones audited).
- `--host`/`--port` must pass `enforce(net_listen)` (loopback, or any address with `net.mode` `any`). `--watch` needs `exec` (it re-executes forge-serve); a policy that denies exec stops forge-serve at startup.
- After the socket is bound and the model loaded, forge-serve calls `security::lockdown(policy)` (SBX-1) before it accepts the first request, prints controls that are not `ok` to stderr, and audits the lockdown. Linux: seccomp is synchronised to every thread (TSYNC); Landlock write rules bind the main thread and the request threads started after it, not the scheduler thread started before (it is covered by `enforce()` only). `--kv-spill` needs its spill directory under `fs.write`.
- `forge-serve proxy --policy` checks the listen address and every `--backend` / `--route` target (`net_connect`; `net.mode` `none` refuses them all), then locks down after the listener starts.

Test: `tests/security_cli_test.py` (strict policy with a signed model serves on loopback; an unsigned model and a non-loopback `--host` are refused).

### Endpoints

| Method and path | Response |
|---|---|
| `GET /health` | `{"status":"ok"}` |
| `GET /v1/models` | `{"object":"list","data":[{"id":<model file name>,"object":"model","owned_by":"forge"}]}` |
| `POST /v1/completions` | `{"id","object":"text_completion","created","model","choices":[{"index","text","tokens","finish_reason"}],"usage"}` |
| `POST /v1/chat/completions` | `{"id","object":"chat.completion",...,"choices":[{"index","message":{"role":"assistant","content"},"finish_reason"}],"usage"}` |
| `GET /metrics` | Prometheus text, see below |
| `OPTIONS *` | `204` with permissive CORS headers. All responses carry `Access-Control-Allow-Origin: *` |

The method is only checked on the completion endpoints (`405` otherwise). Other paths get `404`.

### Request fields

| Field | Default | Notes |
|---|---|---|
| `prompt` (completions) | required | A string (BOS is prepended if the tokenizer has one) or a list of token ids (sent as is) |
| `add_bos` (completions) | absent | With a string prompt: `false` skips the BOS |
| `messages` (chat) | required | `[{role, content}]`. There is no chat template: the prompt is `"<role>: <content>\n"` per message plus `"assistant:"`, with BOS prepended |
| `max_tokens` | 16 | |
| `temperature` | 0 | Greedy by default |
| `top_k`, `top_p`, `min_p`, `typical_p` | 0, 1, 0, 1 | |
| `repetition_penalty`, `presence_penalty`, `frequency_penalty` | 1, 0, 0 | |
| `mirostat_tau`, `mirostat_eta` | 0, 0.1 | |
| `seed` | 0 | |
| `stop` | none | String or list of strings. Output is cut before the first match |
| `grammar` | none | GBNF text (see [inference.md](inference.md)) |
| `response_format` | none | `{"type":"json_schema","json_schema":{"schema":{...}}}` is converted to a grammar (overrides `grammar`). Output is compact JSON |
| `stream` | false | Server-Sent Events |

`finish_reason` is `stop` (EOS, stop string, grammar complete or a token the grammar rejected), `length`, `cancelled`, or `error: <message>`. `usage` has `prompt_tokens`, `completion_tokens`, `total_tokens`. Text is sanitised to valid UTF-8 (invalid bytes become U+FFFD). EOS is not included in `text` or `tokens`.

Streaming sends `data: {...}` events: completion chunks carry `choices[0].text` and `token`, chat chunks `choices[0].delta.content`. A token that ends inside a UTF-8 sequence is held back until the sequence completes. The last event has `finish_reason` and `usage`, followed by `data: [DONE]`. A streaming client that disconnects cancels its request.

Errors are JSON `{"error":{"message","type":"invalid_request_error"}}`: `400` for bad requests, `503` for a full queue or an out-of-memory error while handling the request.

### Metrics
`forge_steps`, `forge_completed`, `forge_prefix_hits`, `forge_prefix_tokens_reused`, `forge_peak_active`, `forge_peak_kv_bytes`, `forge_queue_waiting`, `forge_queue_active`, `forge_admission_deferred_total`, `forge_rejected_total`, `forge_kv_evictions_total`, `forge_kv_spills_total`, `forge_kv_reloads_total`, `forge_kv_spilled`, then the memory governor's `forge_mem_*` series ([memory.md](memory.md)).

## Examples

```sh
forge-serve golden/tiny_f32.fpk --port 18093 &
curl -s localhost:18093/health
curl -s localhost:18093/v1/completions -d '{"prompt":"Hello","max_tokens":4}'
curl -s localhost:18093/v1/chat/completions -d '{"messages":[{"role":"user","content":"hi"}],"max_tokens":3}'
curl -s localhost:18093/v1/completions -d '{"prompt":"x","max_tokens":8,"response_format":{"type":"json_schema","json_schema":{"schema":{"enum":["yes","no"]}}}}'
curl -sN localhost:18093/v1/completions -d '{"prompt":"Hello","max_tokens":8,"stream":true}'
curl -s localhost:18093/metrics
```

The `serve_http` ctest (`tests/serve_test.py`) checks concurrent completions against `forge run`, streaming, chat, grammar, JSON Schema, stop strings, and a tiny `--mem-limit` with `--max-queue`. `--kv-spill` itself is covered at the scheduler level in `tests/test_memory.cpp`, not over HTTP.

### systemd unit

    [Service]
    Type=notify
    ExecStart=/usr/local/bin/forge-serve /srv/model.fpk --port 8080 --watch
    Environment=FORGE_HUGEPAGES=1

## Platform notes and limits
- **No authentication or TLS.** It binds to 127.0.0.1 by default; put a reverse proxy in front before exposing it.
- No HTTP keep-alive; headers are limited to 1 MiB and bodies to 64 MiB.
- An exception inside the scheduler (for example a prompt longer than `--ctx`) ends every active request with `finish_reason` `error: ...`, returned with status 200.
- Request priority and stop tokens by id are scheduler features with no HTTP field.
- `--ask-model` is a `forge script` flag, not a forge-serve flag (see [scripting.md](scripting.md)).
- Readiness notification was tested with an abstract `NOTIFY_SOCKET` in `test_os_ext`, not under a real systemd. Running under the Windows SCM or launchd is untested.
