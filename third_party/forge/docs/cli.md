# The `forge` command-line tool

Source: `apps/forge.cpp`. Run `forge` with no arguments for the built-in usage text. The usage text is shorter than this page; where they differ, this page follows the code.

## Concepts
- `forge <command> [arguments]`. Commands: `info`, `import`, `inspect`, `run`, `bench`, `dump`, `keygen`, `sign`, `verify`, `encrypt`, `rpc-serve`, `security`, `script`.
- Options (all commands except `script`) are `--name value`, `--name=value` or `-x value`. Positional arguments can go anywhere.
- These options are flags and never take a value: `--verify`, `--tensors`, `--blueprint`, `--ids`, `--no-bos`, `--allow-scripts`, `--script-no-proc`, `--script-no-fs`, `--script-no-net`, `--json`, `--detached`. Any other option takes the next argument as its value, so an option such as `--pure` must be last or written `--pure=1`.
- Unknown options are ignored. An unknown command, or a missing required argument, prints the usage and exits 2. Errors print `forge: error: <message>` and exit 1.
- Wherever a model path is taken by `run` and `bench`, it may be a `.fpk`, a Hugging Face directory, a `.gguf` or an `.onnx` file. Non-`.fpk` inputs are imported once into `$FORGE_CACHE` (default `~/.cache/forge`) with `--codec` (default `f32`) and reused while the source files are unchanged.

## Commands

### `forge info`
Prints the CPU name and feature flags (`cpu`, `features`), the simd kernel table in use (line `lane`), and one `grid` line per GPU runtime: `cpu`, `cuda`, `hip`, `level_zero` and `vulkan` show whether the runtime library could be loaded (not whether a device works); `metal` always shows `no`. It ends with physical and available RAM, any cgroup or `RLIMIT_AS` limit, and the host memory budget with its source (see [memory.md](memory.md)).

### `forge import <hf_dir | model.gguf | model.onnx> -o out.fpk [options]`
Converts a model to `.fpk`.

| Option | Meaning |
|--------|---------|
| `--codec C` | Weight codec (default `f32`): `f32 f16 bf16 fp8_e4m3 fp8_e5m2 q8_0 q4_k q5_k q6_k q4_g32 q8_g32 mxfp8 mxfp6 mxfp4 mxint8 nvfp4 tq2`, or the mixed-precision presets `q4_k_m` / `q5_k_m` |
| `--emb-codec C` | Codec for the token embedding (default `q8_0` when `--codec` is quantised, else the same as `--codec`) |
| `--template NAME` | Blueprint template from `import/templates/`: `llama`, `mixtral`, `deepseek_v2`, `qwen3`, `qwen3_5`. Normally chosen from the model's architecture |
| `--recipe forge\|llama` | Mixed precision: `llama` is a port of llama.cpp's per-tensor rules; `forge` (default) adds rules for DeltaNet and attention-output projections |
| `--output-tensor-type T`, `--token-embedding-type T` | Mixed precision: codec for the output head / token embedding |
| `--tensor-type REGEX=TYPE[,REGEX=TYPE...]` | Mixed precision: per-tensor overrides |
| `--pure` | Mixed precision: no per-tensor rules, one codec for all |
| `--bf16-head` | Mixed precision: keep the output head in bf16 |

The mixed-precision options are documented for Hugging Face inputs. Templates are read from the source tree compiled into the binary; set `FORGE_TEMPLATES` for another directory (for example with an installed binary).

### `forge inspect <model.fpk> [--verify] [--tensors] [--blueprint]`
Prints the manifest, the section table (offset, size, BLAKE3 prefix) and a blueprint summary. `--tensors` lists every tensor with its codec and shape, `--blueprint` prints the graph text, `--verify` checks every hash and exits 1 on a mismatch. Works on encrypted files without the key (metadata stays readable).

### `forge run <model> (--prompt TEXT | --tokens 1,2,3) [options]`

| Option | Default | Meaning |
|--------|---------|---------|
| `-n N` | 32 | New tokens to generate |
| `--temp T` | 0 | Temperature (0 = greedy) |
| `--top-k`, `--top-p`, `--min-p` | 0, 1, 0 | Sampling filters (the defaults are "off") |
| `--repeat-penalty` | 1 | Repetition penalty |
| `--seed` | 0 | RNG seed |
| `--ctx N` | 0 | Context length; 0 = model maximum |
| `--ids` | off | Print token ids instead of text (always on when the model has no tokenizer) |
| `--no-bos` | off | Do not prepend the BOS token |
| `--logits FILE` | | Write the prefill logits as raw f32 |
| `--verify` | off | Check BLAKE3 hashes on open |
| `--grammar FILE.gbnf` | | Constrain output with a GBNF grammar (needs a tokenizer) |
| `--draft draft.fpk` | | Speculative decoding with a draft model |
| `--lookup [--ngram 3] [--ngram-min 1]` | | Speculative decoding with n-gram (prompt-lookup) drafts |
| `--spec-k N` | 0 | Drafts per pass; 0 = chosen per pass by the planner. Ignored with `--grammar` (plain decoding) |
| `--codec C` | `f32` | Codec for on-the-fly import of non-`.fpk` inputs |
| `--policy FILE` | | Security policy (see [Security policy](#security-policy)): the model must be readable under `fs.read` and pass `integrity.models` |

Generated text goes to stdout; timing (prompt and new tokens, tok/s, plans compiled and reused) goes to stderr.

Models whose blueprint session is not `generative_sequence` (for example imported ONNX graphs) run as one-shot inference instead: pass `--input name=file.f32` (raw little-endian f32, repeatable) and optionally `--out PREFIX` to write `PREFIX.<output>.f32`. Without `--out` the outputs are printed.

### `forge bench <model> [-p 32] [-n 32] [--warmup 1] [--codec C]` / `forge bench --gemm N`
Measures prefill (`-p` prompt tokens) and decode (`-n` new tokens, always all of them) throughput after `--warmup` untimed runs. With `--gemm N` it instead times an N×N `gemm_nt` with each available simd table (one thread) and with `gpu::prims` (all threads).

On a CUDA or HIP device it also prints driver calls for the prefill and per decode token: kernels launched directly, kernels captured into a CUDA graph (on by default; `FORGE_GRAPHS=0` turns graphs off), graph launches, graph instantiations, kernel-node updates, replayed runs, DtoH/HtoD/DtoD copies and stream syncs. `tools/bench_llm.sh` wraps it for a side-by-side run against llama.cpp.

### `forge dump <model.fpk> <tensor> <out.f32>`
Decodes one tensor to raw little-endian f32.

### `forge keygen -o NAME`
Writes an Ed25519 key pair: `NAME.key` (secret seed, owner read/write only) and `NAME.pub`.

### `forge sign <model.fpk> --key NAME.key [-o out.fpk]`
Adds a signature, or appends one to an existing signature section. Without `-o` the input file is rewritten.

### `forge sign <file> --key NAME.key [-o file.sig] [--name NAME] [--detached]`
Any file that is not a `.fpk` (plugins `.so` / `.dll` / `.dylib`, scripts `.fs` / `.cpp`, ...), or any file with `--detached`: writes the detached signature `<file>.sig` (or `-o`), leaving the file untouched. An existing `.sig` for the same name and content is extended (several signers); one by the same key is replaced. The signature binds the file's base name; `--name` signs it under another name (for a remote script whose `forge::remote` `name` differs). Format: [modelfile.md](modelfile.md#detached-signatures).

### `forge verify <model.fpk> [--pubkey NAME.pub]...`
Checks every section hash and the signatures. Prints `VERIFIED` and exits 0, or prints the failure and exits 1. Without `--pubkey` any valid signature is accepted (integrity only), so an unsigned file fails. With one or more `--pubkey`, at least one valid signature must come from a listed key.

### `forge verify <file> [--sig file.sig] [--pubkey NAME.pub]... [--detached]`
Checks a detached signature: `<file>.sig` (or `--sig`) must name this file's base name and match its BLAKE3 digest and size, and carry a valid signature (from a listed key when `--pubkey` is given). Prints `signature ok key <id>` / `signature BAD key <id>` per signer and `<file>: VERIFIED`, or the reason (`unsigned`, `content does not match the signed digest`, `signature is for 'x', not 'y'`, `not by a trusted key`), exit 0 or 1.

### `forge encrypt <in.fpk> -o out.fpk (--key K.mkey | --new-key K.mkey)`
Seals the tensor data with XChaCha20-Poly1305. `--new-key` creates the key file (64 hex digits, mode 0600 where supported). Load encrypted models with `FORGE_MODEL_KEY_FILE=K.mkey` or `FORGE_MODEL_KEY=<hex>`.

### `forge security check [--policy FILE] [--json]`
Loads the security policy (`--policy`, `$FORGE_SECURITY_POLICY`, `./forge-security.json`, `<config dir>/forge/forge-security.json`; `$FORGE_SECURITY_PROFILE` picks a base profile) and prints, for each FORGE Security Standard control, its state (`active`, `available`, `emulated`, `missing`, `n/a`, `failed`), backend, level and verdict. It changes nothing. Exits 0 when every required control can be met, 1 when one cannot, 2 when the policy is invalid. With no policy and no profile it reports "no FSS profile active" and exits 0. Details: [security-api.md](security-api.md).

### `forge rpc-serve [options]`
Lends a local device to other processes as a remote `rpc:host:port` device (see [graph.md](graph.md)), and optionally runs remote scripts. Details: [rpc.md](rpc.md).

| Option | Default | Meaning |
|--------|---------|---------|
| `--port N` | 7000 | Listen port |
| `--bind ADDR` | `127.0.0.1` | Listen address. Without authentication on a non-loopback address it prints a warning |
| `--device D` | `cpu` | Device to serve (`FORGE_DEVICE` syntax); falls back to the CPU if unavailable |
| `--token T` | | Accept this bearer token, full access (repeatable); `--token 'allow=device T'` limits it |
| `--token-file F` | | One token per line: `[allow=device,scripts] <token> [name]`; a line without `allow=` has full access (repeatable) |
| `--authorized-keys FILE` | | One key per line: `[allow=device,scripts] <64-hex key> [comment]`; a line without `allow=` has full access (repeatable) |
| `--pubkey HEX\|FILE` | | Accept this Ed25519 public key (repeatable) |
| `--tls-cert C --tls-key K` | | Serve TLS (needs a build with OpenSSL). Both or neither |
| `--tls-client-ca CA.pem` | | Require a client certificate signed by this CA (mTLS); needs `--tls-cert`/`--tls-key` |
| `--tls-client-map FILE` | | Accept only these client certificates: `[allow=...] sha256:<fingerprint>` or `cn:<name>` per line; permissions intersect with the token's or key's (repeatable) |
| `--quiet` | off | No security log on stderr |
| `--allow-scripts` | off | Run scripts sent by authenticated clients |
| `--script-timeout MS` | 60000 | Upper limit per script |
| `--script-mem-mb N` | 2048 | Memory limit per script process |
| `--script-no-proc`, `--script-no-fs`, `--script-no-net` | off | Deny those capabilities to remote scripts |
| `--policy FILE` | | Security policy: the listen address must pass `net_listen`, `--allow-scripts` needs `exec`, and the server calls `lockdown()` after binding, before the first request; script children get the same policy ([rpc.md](rpc.md#security-policy)) |

Without tokens or keys the server is open to anyone who can reach it. Each remote script runs in its own `forge script` child process with the OS sandbox set to `require` (`best-effort` on Windows) when any `--script-no-*` flag is given. Over TLS, token and key authentication are bound to the TLS connection (RFC 9266 `tls-exporter`). Clients take their settings from the `FORGE_RPC_*` variables ([configuration.md](configuration.md#rpc-clients)).

### `forge script`

```
forge script [options] <file.cpp> [args...]   run a script; int main()'s return value is the exit status
forge script [options] -e '<code>' [args...]  run a snippet
forge script [options]                        interactive REPL
forge script --emit <target> <file.cpp> [-o out]   translate to a standalone shell script
forge script --emit-targets                   list the shells that have an emitter
forge script --check [--json] [--strict-check] [--module-dir DIR]... <file.cpp>   static checks only
```

| Option | Meaning |
|--------|---------|
| `--tree` | Tree-walking interpreter only (default: compile to verified bytecode) |
| `--debug` | Race detector, lock-order and stall deadlock detection |
| `--timeout MS`, `--max-steps N`, `--heap BYTES` | Wall-clock limit, instruction budget, managed heap cap |
| `--no-proc`, `--no-fs`, `--no-net` | Capability sandbox |
| `--os-sandbox off\|best-effort\|require` | How the capability flags are also enforced by the OS sandbox |
| `--policy FILE` | Security policy: capabilities from the policy, file operations path-scoped to its roots, `lockdown()` before the script runs ([scripting.md](scripting.md), "Security policy") |
| `--no-check` | Skip the static checks that run before a script |
| `--check`, `--json`, `--strict-check`, `--module-dir DIR` | Static checks only. Diagnostics go to stderr, or to stdout as JSON with `--json`. Exit 2 on errors, or on any diagnostic with `--strict-check`. `--module-dir` adds an import directory |
| `--cache-dir DIR`, `--cache-trust HEX`, `--cache-require-signed`, `--cache-sign-seed HEX` | Compiled-script cache: directory, trusted public keys, refuse unsigned entries, signing seed |
| `--ask-model M.fpk`, `--ask-max-tokens N` | Run `forge::ask` and related calls on this model in-process (defaults `$FORGE_ASK_MODEL`, `$FORGE_ASK_MAX_TOKENS` or 256) |
| `--emit TARGET`, `-o FILE` | Translate instead of running. `TARGET` is a shell from `--emit-targets`, `auto` (`$FORGE_SHELL` or the host shell), `android` (mksh), or `ast` |
| `--` | End of options; the next argument is the script file |

- Scripts are an interpreted, memory-safe subset of C++23; see [scripting.md](scripting.md). Arguments after the file are available as `forge::args()`.
- REPL commands: `:help`, `:type`, `:dis`, `:mem`, `:gc`, `:vm on|off`, `:reset`, `:quit`.
- Code drafted by AI calls (`forge::ai_run` / `ai_eval`) runs only after you answer `y` on the terminal.
- `--emit` prints to stdout, or writes `-o FILE` and marks it executable. A construct the target cannot express exits 2 and prints `file:line: emit <target>: <reason>`. See [scripting.md, "Write once, emit anywhere"](scripting.md#write-once-emit-anywhere).
- A bad option or unreadable file prints `forge script: <message>` and exits 2.

## Strict loading
`FORGE_REQUIRE_SIGNED=1` refuses unsigned models; `FORGE_TRUSTED_KEYS=<hex>[,<hex>]` only accepts signatures from those public keys. Both apply to every command that loads a model through `inference`.

## Security policy
`forge run`, `forge bench`, `forge script`, `forge rpc-serve` and `forge-serve` take `--policy FILE`. Without it they use what `forge security check` would: `$FORGE_SECURITY_POLICY`, `./forge-security.json`, `<config dir>/forge/forge-security.json`, or `$FORGE_SECURITY_PROFILE` alone. With none of these (profile `none`) nothing changes. With a policy:

- every model load through `inference` checks `fs.read` for the path and `integrity.models`: `required` needs a valid signature (by one of `integrity.trusted_keys`, if any) and fails with `IntegrityError` / `security::integrity_error` before the blueprint or any tensor is used; `optional` verifies signed files and audits unsigned ones. `FORGE_REQUIRE_SIGNED` / `FORGE_TRUSTED_KEYS` still apply on top;
- `forge script` takes its capabilities from the policy and locks down before running; `integrity.scripts` checks the script file's and its imports' detached `.sig`, and `required` refuses `-e` code ([scripting.md](scripting.md), "Security policy");
- native plugins follow `integrity.plugins` (detached `.sig`) and `integrity.os_code_signing`;
- the servers check the listen address, call `lockdown()` after binding and loading, and audit it ([rpc.md](rpc.md#security-policy), [serve.md](serve.md#security-policy)).

A bad or unreadable policy file is an error (exit 1, or 2 for `forge script`). See [security-api.md](security-api.md).

## Example session
These commands were run against the golden model on Linux x86_64 in an earlier revision; they were not re-run for this revision of the page:

```sh
B=build/dev
$B/forge info
$B/forge inspect $B/golden/tiny_f32.fpk
$B/forge run $B/golden/tiny_f32.fpk --prompt "Hello" -n 8
$B/forge bench $B/golden/tiny_f32.fpk -p 8 -n 8
$B/forge keygen -o dk
$B/forge sign $B/golden/tiny_f32.fpk --key dk.key -o s.fpk
$B/forge verify s.fpk --pubkey dk.pub                 # ... s.fpk: VERIFIED
$B/forge encrypt s.fpk -o sealed.fpk --new-key k.mkey
FORGE_MODEL_KEY_FILE=k.mkey $B/forge inspect sealed.fpk
```

`tests/sign_test.py` (ctest `modelfile_signing`) runs `keygen`, `sign`, `verify` and `encrypt` on every test run.

## Platform notes
- `forge encrypt` sets the new key file's mode with `chmod 0600`, which has no effect on Windows.
- `forge rpc-serve` TLS options need a build with OpenSSL (configure prints `FORGE: rpc TLS via OpenSSL ...`).
