# Security model

What FORGE enforces today and where. Checked against the source on 2026-11-04 (main at ccc0a7d).
For the target standard see [security-standard.md](security-standard.md); its API is [security-api.md](security-api.md).

## Script capabilities (interpreter)

| Item | Where |
|---|---|
| `struct Caps { fs, net, proc, env, reflect }`, all on by default | `script/src/interp.hpp` |
| Narrow with `forge::sandbox(opts, fn)`; it can only turn flags off | `script/src/builtins.cpp` |
| Narrow from the CLI: `forge script --no-fs --no-net --no-proc` | `apps/forge.cpp` |
| Under a security policy: capabilities from the policy, path / host scoping via `enforce_scope` | `apps/forge.cpp` (`script_policy`), `script/src/builtins.cpp` |
| Checks raise `PermissionError` | `need()` in `builtins.cpp`, `need_dev()` in `mobile.cpp`, `need_reflect()` in `reflect.cpp`, plus checks in `runtime.cpp`, `ai.cpp`, `remote.cpp`, `text.cpp`, `automation.cpp` |

- `env` gates `std::getenv` and device access (`forge::mobile`, sensors).
- `reflect` gates `reflect::set/invoke/call/construct`.
- Files and processes opened before a narrower `sandbox` block cannot be read, written or killed inside it.
- These are interpreter checks. A bug in the interpreter or a native built-in can bypass them, so the OS
  sandbox below backs them up.
- Escape tests: `tests/script/sandbox_escape.fs` and `capability_escape_engine_level` in `tests/test_script.cpp`.

### OS sandbox for scripts

When `--no-fs`, `--no-net` or `--no-proc` is given, the engine also applies the OS sandbox
(`script/src/engine.cpp`): `--no-net` → deny network, `--no-proc` → deny exec, `--no-fs` → deny file writes
(reads stay allowed). `--os-sandbox off|best-effort|require` picks what happens if it cannot be applied;
`require` aborts the run.

`sandbox({..., isolate: true}, fn)` runs the block in a forked child under the narrower OS sandbox and returns
its result as JSON (`builtins.cpp`). Without `fork()` (Windows) it raises `PermissionError` and does not run
the block. Plain nested `sandbox` blocks are interpreter-checked only.

### Static capability checks

Before a run, the static checker (docs/scripting.md "Static checks", `script/src/check.cpp`) reports direct calls
to `fs`, `net`, `proc`, `service` or environment built-ins, `run()` and shell blocks that the run's capabilities
or an enclosing literal `sandbox({...}, fn)` deny. They are errors, so the script is refused before it starts.
Indirect calls (a built-in in a variable, `eval`, imported modules) are not seen; the runtime checks remain the
enforcement.

## OS sandbox (`os/include/forge/os/sandbox.hpp`)

API: `sandbox_policy { deny_network, deny_exec, deny_fs_write }`, `apply_sandbox`, `run_sandboxed`,
`sandbox_status`. Applying is one-way.

| Platform | `apply_sandbox` (current process) | `run_sandboxed` (child) |
|---|---|---|
| Linux, Android | `no_new_privs` + seccomp-BPF, EPERM for denied syscalls (io_uring, x32 and i386 `socketcall` closed; the `*at` file-write calls and `*setxattr` denied); `unsupported` on architectures with no audit arch. Landlock stacked on top when the kernel has it (5.13+, ABI 1-6 detected; raw syscalls): `deny_fs_write` handles every write right of the ABI (refer from 2, truncate from 3) with no rule allowing it, `deny_exec` handles EXECUTE, `deny_network` handles TCP bind/connect from ABI 4. Without Landlock, seccomp alone | fork, Landlock + seccomp, exec; with Landlock `deny_exec` applies to the target (see note) |
| FreeBSD | Capsicum `cap_enter()` (all-or-nothing, coarser than asked) | fork, open the binary, `cap_enter`, then `fexecve` (static) or `ld-elf.so.1 -f` with `LD_LIBRARY_PATH_FDS` (dynamic; libraries from the system dirs and `/usr/local/lib` only); scripts refused |
| OpenBSD | `pledge()` | fork, apply, exec |
| macOS | `sandbox_init` named profiles, found with `dlsym` | fork, apply, exec |
| iOS | no-op; the app sandbox already applies | unsupported |
| Windows | exec denial (job object, one active process); net/fs denial returns `unsupported` unless the process already runs in an AppContainer without network capabilities (`process_pool` workers) | AppContainer (net/fs) or restricted token (exec only), in a kill-on-close job; arguments quoted like `spawn_wait` |

On POSIX, `run_sandboxed` applies the network and file-write parts in the child before `execvp`. `deny_exec`
cannot go into the seccomp filter (it would stop the target too). On Linux with Landlock it is applied to the
target: EXECUTE is allowed only on the target binary, its `#!` interpreter and its ELF interpreter (`ld.so`);
Landlock rules survive `execve`, so the target runs but cannot start another program. Without Landlock, and on
macOS, the target itself may still exec. `sandbox_status()` reports `mechanism` (`seccomp-bpf+landlock` when this
process applied Landlock), `landlock_abi` (0: unavailable) and `landlock`. `FORGE_LANDLOCK=0` turns Landlock off.
Android: the first probe runs in a throwaway child, since an app's seccomp policy may kill on unknown syscalls.

`process_pool` on Windows starts workers whose policy denies network or file writes inside an AppContainer
(profiles `forge.sandbox.nonet`, `.nofs`, `.nonet.nofs`, shared with `run_sandboxed`). The worker inherits only
its two pipe ends and stderr (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). The pool adds an ACE granting the profile
read + execute on the worker image, because AppContainers cannot read files in user directories by default. The
worker checks its token (AppContainer, no `internetClient` / `internetClientServer` /
`privateNetworkClientServer` capability when the network is denied) before recording the policy. Loopback is
blocked too. Writes are allowed only in the container folder (`%LOCALAPPDATA%\Packages\<profile>\AC`).

Tests: the `sandboxing` case of `tests/test_os_ext.cpp` runs in CI on Linux, macOS, Windows and FreeBSD
(sub-cases that report `unsupported` skip; FreeBSD skips when Capsicum refuses a partial policy).
`run_sandboxed` is tested on Linux (Landlock target exec denial, `#!` and dynamic targets), FreeBSD and Windows (AppContainer included) in the `run_sandboxed_child` case of `tests/test_os_ext.cpp`; the `sandboxing` case also applies Landlock alone (EACCES for writes and exec). `tests/test_process_pool.cpp` checks Linux workers report `seccomp-bpf+landlock`, and Windows AppContainer workers run tasks, cannot connect to a loopback listener and cannot write to the temp directory. `process_pool` workers that cannot apply a requested sandbox fail instead of running unsandboxed.

## Native plugins

Native plugins are refused unless fs, net, proc and env are all enabled
(`builtins.cpp`: "native plugins are refused in sandboxed runs").

## AI-drafted code

`forge::ai_run` and `forge::ai_eval` run drafted code only if a confirm callback returns true: per call
`{"confirm", fn}`, else the engine's `confirm`. With neither, `confirmed()` returns false and nothing runs
(`script/src/ai.cpp`). The CLI prompts `[y/N]`. `ai_run` goes through the normal shell built-ins, so capability
flags and child limits still apply.

## RPC (`graph/include/forge/graph/rpc_secure.hpp`, `rpc.hpp`, docs/rpc.md)

| Feature | Behaviour |
|---|---|
| Auth | Tokens (`--token`, `--token-file`) and Ed25519 keys (`--authorized-keys`, `--pubkey`). Tokens are compared in constant time; nonces come from the OS CSPRNG. |
| TLS | OpenSSL, when built with it (`--tls-cert`, `--tls-key`). |
| mTLS | `--tls-client-ca`: every client must present a certificate that chains to the CA. Needs `--tls-cert/--tls-key`. `--tls-client-map` accepts only listed certificates (`sha256:` fingerprint or `cn:`), each with `allow=` permissions; unlisted certificates are refused. With tokens or keys configured a request needs both, and gets the intersection of their permissions; with only a map, a mapped certificate authenticates device ops by itself (never scripts). |
| Channel binding | Over TLS only the bound methods are accepted. The binding is RFC 9266 tls-exporter on TLS 1.3 and on TLS 1.2 with extended master secret, else RFC 5929 tls-unique. A relayed or replayed auth exchange fails; a bound token never crosses the wire. |
| Per-key and per-token permissions | authorized_keys and `--token-file` lines may start with `allow=device`, `allow=scripts` or `allow=all`. Checked per request. Unknown options are errors (with file and line; the server does not start). A line without options, or a bare `--token`, has full access. The server logs each connection's principal (token number and name, key prefix, certificate) and every denial. |
| Remote scripts | `rpc-serve --allow-scripts`. The server refuses to start with it and no tokens or keys. Each script runs in its own child (`forge script --rpc-child`) with a timeout (`--script-timeout`, default 60000 ms), a memory cap (`--script-mem-mb`, default 2048) and optional `--script-no-proc/-fs/-net`. On POSIX the child requires the OS sandbox; on Windows it is best-effort. RPC secrets are removed from the child's environment. |
| No auth | Allowed for non-script RPC. On a non-loopback bind the server prints a warning. |

`forge::remote_call` ships a function and its captured values as an ordinary remote script, so the same auth,
child process, sandbox and limits apply. It refuses anything but plain data (nil, bool, numbers, strings,
lists, maps, script functions) with a `TypeError` (`script/src/remote.cpp`).

Tests: `test_rpc` (auth, `allow=` for keys and tokens, config errors), `test_rpc_tls` (server verify, mTLS, client-certificate
map, channel binding). `test_rpc_tls` runs the `openssl` CLI without a shell, so it no longer depends on a POSIX redirect.

## Model files and signing

| Item | Where |
|---|---|
| `.fpk` signing: Ed25519 over a BLAKE3 digest of the section table (name, size, hash of each non-signature section). CLI `forge keygen/sign/verify`. | `modelfile/include/forge/modelfile/sign.hpp` |
| `verify_signature(reader, trusted)`: with no trusted keys, any valid signature passes (integrity only, not provenance). | `sign.hpp` |
| `.fpk` encryption: the data section sealed with XChaCha20-Poly1305 (vendored Monocypher). `forge encrypt`; key from `FORGE_MODEL_KEY` (hex) or `FORGE_MODEL_KEY_FILE`. Metadata opens without the key. | `modelfile/include/forge/modelfile/fpk.hpp` |
| Signed bundles: Ed25519 over a `FORGE-SIGNATURE.json` manifest of (path, size, BLAKE3) for a directory. Verification re-hashes and reports modified, missing and unsigned extra files; multiple signers; paths that escape the root are refused. Also `sign_bytes`/`verify_bytes` with a domain-separation context. Library API only, no CLI command. Test: `test_bundle_sign`. | `modelfile/include/forge/modelfile/bundle.hpp` |
| Script cache `.fsc`: BLAKE3 key, optional Ed25519 signature (`--cache-dir`, `--cache-trust`, `--cache-require-signed`, `--cache-sign-seed`). Signatures are required when the cache directory is not private. Tests: `tests/test_script_fsc.cpp`, fuzz `fuzz/fuzz_fsc.cpp`. | `script/src/fsc.cpp`, `engine.cpp` |
| Release artefacts | `tools/release.py`, docs/RELEASING.md |

## Secrets at rest (`os/include/forge/os/keystore.hpp`)

Keychain on Apple (items `AfterFirstUnlockThisDeviceOnly`, updated in place), DPAPI on Windows, libsecret on Linux
and FreeBSD when the session bus has a Secret Service provider, an app-installed backend on Android. libsecret
failures (locked keyring, refused prompt) are returned as errors; the file backend is used only when there is no
bus or provider, or with `FORGE_KEYSTORE=file`, and `keystore::backend()` reports which one is in use. Android has
no default directory: the JNI glue sets `FORGE_KEYSTORE_DIR` or installs a backend.

The file backend seals each secret with XChaCha20-Poly1305 (Monocypher) under a random 32-byte store key, with a
random nonce and the (service, account) pair as associated data, so a modified, truncated or swapped file fails
`get()` instead of returning garbage. Files are created 0600 from the start (`O_EXCL`, no symlink following) and
replaced atomically; the store key file `.master` is created exclusively, so concurrent processes agree on one key,
and is DPAPI-wrapped on Windows. A directory FORGE creates is 0700; an existing `FORGE_KEYSTORE_DIR` keeps its mode.
Unwrapped, the key sits beside the secrets, so this protects against other users and against copies of single
files, not against code running as the same user. Old XOR-pad files are re-encrypted on first read and the pad is
deleted once none remain.

The store key can be wrapped; `keystore::backend()` then reports `file+argon2id`, `file+tpm` or
`file+tpm+argon2id`. Wrapping applies to the file backend only (default on Linux/FreeBSD without a Secret Service
and on Android with `FORGE_KEYSTORE_DIR`; `FORGE_KEYSTORE=file` elsewhere).

- **Passphrase** (`FORGE_KEYSTORE_PASSPHRASE`, `keystore::set_passphrase()`, or `set_passphrase_provider()` for a
  prompt). `.master` holds the key sealed with XChaCha20-Poly1305 under Argon2id(passphrase, 16 random bytes of
  salt). The Argon2 parameters are stored in the header and authenticated with it (`keystore::set_kdf`, default
  64 MiB / 3 passes / 1 lane; at least 8 MiB and 2 passes, and at most 1 GiB, 64 passes and 16 lanes, both when
  set and when read back, so a tampered header cannot request a huge allocation). A wrong passphrase or a modified
  header fails with `EACCES`, never with a wrong key. A plain `.master` is wrapped the first time a passphrase is
  supplied. `keystore::rewrap(old, new)` changes the passphrase, or removes it when `new` is empty; the secrets are
  not re-encrypted. On Windows the wrapped key is also DPAPI-protected.
- **TPM** (opt-in, `FORGE_KEYSTORE_TPM=1`; off by default because a cleared or replaced TPM loses the key). On
  Linux and FreeBSD, tpm2-tss is loaded at run time (`libtss2-esys.so.0`, `libtss2-mu.so.0`,
  `libtss2-tctildr.so.0`, using `/dev/tpmrm0`; the user needs access to it, usually through the `tss` group). An
  SRK-template primary (ECC P-256, or RSA-2048 if that fails) under the owner hierarchy, with an empty owner
  password, parents a keyedhash sealed object (`noDA`, no PCR policy). Its public and private blobs are stored in
  `.master.tpm`. On Windows the key is RSA-OAEP-encrypted with a non-exportable RSA-2048 key, per store, in the
  Microsoft Platform Crypto Provider (NCrypt, TPM-backed). `.master.tpm` names that key and holds the ciphertext.
  If `FORGE_KEYSTORE_TPM=1` is set and no TPM is usable, the file backend fails with an unsupported error;
  `keystore::tpm_available()` says what is missing. `FORGE_KEYSTORE_TPM_TCTI` overrides the TCTI, e.g. for
  libtpms or swtpm in tests.
- **Recovery.** With the TPM alone, `.master` is only a marker and the key exists only in the TPM-sealed copy. Set a
  passphrase as well and `.master` keeps a passphrase-wrapped copy. The TPM copy is tried first; if it cannot be
  unsealed (TPM cleared, motherboard replaced, store copied to another machine), the passphrase still opens the
  store. `keystore::disable_tpm()` unseals the key, drops `.master.tpm` and deletes the NCrypt key.

The unwrapped key is cached in process memory, keyed by directory, key files and passphrase, so Argon2 and the TPM
run once per process; `keystore::clear_passphrase()` wipes the cache. Derived keys, Argon2 work areas and key
buffers are wiped after use.

## Build hardening (`cmake/forge_hardening.cmake`)

`FORGE_HARDENING` (default ON) adds compiler and linker hardening, probing each flag and skipping what the toolchain
rejects; the flag list per toolchain is in docs/building.md. `forge::build_hardening()`
(`core/include/forge/core/build_info.hpp`) returns `{name, enabled}` for each property. MSVC builds do not use `/sdl`
(security-standard.md BLD-1).

## Exceptions

`Interp::from_cpp` maps C++ exceptions to script exceptions and `engine` catches all of them, so an embedder never
sees a C++ exception from `run`/`eval` (docs/scripting.md, Errors).

## Known gaps

- RPC: permissions are coarse (`device`, `scripts`); client-certificate rules match only the fingerprint or subject CN
  (no SANs, no revocation list).
- Windows: no in-process network or file-write denial; scripts there get only the job object plus the
  interpreter checks (script `ProcessPool` workers do get an AppContainer).
- `run_sandboxed` does not apply `deny_exec` to the target on macOS, or on Linux without Landlock. With Landlock
  the target can still run another program by invoking the dynamic loader explicitly (`ld.so prog`), since
  Landlock does not govern `mmap(PROT_EXEC)`.
- Windows AppContainer pool workers: the ACE added to the worker image stays (it names only the FORGE profile);
  the profiles persist per user like any package profile. Some CI images refuse AppContainer profiles.
- `sandbox({isolate: true})` needs `fork()`, so it is unavailable on Windows.
- Nested non-isolated `sandbox` blocks are interpreter-checked only.
- Static capability checks miss indirect calls.
- Keystore file backend: unless a passphrase or the TPM wraps the store key, same-user code can read it. TPM
  sealing has no PCR policy, so it binds the key to the machine but not to a boot state, and a process running as
  the same user can still ask the TPM to unseal it.
- `forge-serve --kv-spill` writes KV-cache data (derived from prompts) unencrypted to disk: 0600 files created
  exclusively in a private (0700 / owner-only DACL) directory, `FORGE_MEM_SPILL_DIR` or a fresh
  `<tmp>/forge-spill-<pid>-<random>`, capped by `FORGE_MEM_SPILL_MAX`, deleted when the spill store is destroyed.
  Root / Administrators and a crash that leaves the files behind still expose them.
- Key memory (FSS-CRY-2): locking is best effort (RLIMIT_MEMLOCK, working-set quota) and reported; macOS has no
  per-range core-dump exclusion and Windows excludes the pages only from WER reports, not from debuggers or
  `MiniDumpWriteDump`. The TLS private key lives in OpenSSL's memory, `model_key` (`.fpk` encryption) and the keystore
  passphrase in ordinary memory; short-lived copies are wiped, not locked.
- Private files (FSS-FIO-2) are checked when a key, token or policy file is loaded, not continuously; the check and the
  open are separate steps (a same-user attacker could swap the file in between). Files that are not keys (models,
  scripts, the `.fsc` cache, imported weights) are scoped by `enforce`, not by ownership. Only profile strict refuses;
  other profiles log a warning.
- `enforce` covers the paths FORGE's own code routes through it (see [security-api.md](security-api.md#integration));
  C++ embedders calling `os::read_file`, `std::ifstream` or `modelfile::writer` directly are not checked.
- `forge::security` ([security-api.md](security-api.md)) has the policy file, `enforce`, `lockdown` and `forge security
  check`. FORGE uses it only when a policy is active (`--policy`, a policy file or `FORGE_SECURITY_PROFILE`): then
  `forge script` takes its capabilities from the policy, scopes script file / net / exec / env built-ins through
  `enforce` and locks down before running; `forge-serve` and `forge rpc-serve` lock down after binding; model loads
  check `integrity.models`; plugin loads `plugin_load`; `.fpk` readers, importers, the `.fsc` cache and the KV spill
  directory check `fs_read` / `fs_write`; key, token and policy files are checked for private permissions (refused in
  strict) ([security-api.md, "Integration"](security-api.md#integration)). Without a policy none of this applies,
  except the private-file warnings and the private spill / temp files.
