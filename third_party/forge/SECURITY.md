# Security

## Reporting
Report vulnerabilities privately to the author, Odin Loch (odin.loch@outlook.com.au), not in a public issue.

## Threat model
- **Model files are untrusted input.** The `.fpk` reader bounds-checks every section and tensor against the file size before use, and the parsers (fpk, blueprint, GGUF, safetensors, ONNX, tokenizer, grammar, JSON, script) are fuzzed (ctest label `fuzz`). `forge inspect --verify`, `forge run --verify` and `model::open(path, true)` re-hash every section and tensor (BLAKE3).
- **Signatures.** `forge sign` / `forge verify` use Ed25519 over the file's section digests. `forge verify` without `--pubkey` still requires a valid signature, so an unsigned file fails; with `--pubkey`, a valid signature must come from a listed key. At load time, `FORGE_REQUIRE_SIGNED=1` refuses unsigned models and `FORGE_TRUSTED_KEYS` pins the signers. Without them, a signed file is checked only when it is opened with verification.
- **Encryption.** `forge encrypt` seals tensor data with XChaCha20-Poly1305. Keep keys out of the repository and, where possible, out of the process environment (prefer `FORGE_MODEL_KEY_FILE` to `FORGE_MODEL_KEY`).
- **Network.** There is no telemetry. The runtime opens listening sockets only when you run `forge rpc-serve` or `forge-serve` (server or proxy). Both bind to 127.0.0.1 by default.
  - `forge-serve` has no authentication or TLS. Do not expose it on an untrusted network without a reverse proxy or firewall.
  - `forge rpc-serve` supports bearer tokens, Ed25519 key authentication with per-key permissions, TLS and mTLS (TLS needs a build with OpenSSL). Without tokens or keys it is open to anyone who can reach it, and it warns when bound to a non-loopback address that way. Remote scripts are refused unless the server has authentication and `--allow-scripts`; each runs in its own sandboxed child process. See [docs/rpc.md](docs/rpc.md).
- **Scripts.** `forge script` capability flags (`--no-fs`, `--no-net`, `--no-proc`) are also enforced by the OS sandbox where the platform allows. Code drafted by AI calls runs only after confirmation on the terminal.
- **Plugins and dlopen.** GPU runtimes load system driver libraries (libvulkan, libcuda, libamdhip64, OpenCL, Level Zero) at run time. Native script plugins are loaded code with full process rights. `FORGE_LIB` lets the Python binding load a library from any path. Treat all of these as trusted configuration.

What is enforced where, and the known gaps: [docs/security.md](docs/security.md#known-gaps).
