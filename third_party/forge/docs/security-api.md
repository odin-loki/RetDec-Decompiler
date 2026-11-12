# `forge::security` API

The C++ side of the [FORGE Security Standard](security-standard.md) (FSS). Header-only, target `forge_security`
(`security/include/forge/security/`, umbrella header `security.hpp`). What FORGE enforces outside this module is in
[security.md](security.md).

**Default: no profile.** With no policy file and no `FORGE_SECURITY_PROFILE`, `load_policy()` returns
`profile::none`: nothing is confined, `enforce()` allows everything and audits nothing, `lockdown()` does nothing.
This is FORGE's behaviour before the standard. The draft says "no file ⇒ `strict`"; making strict the default for
every FORGE program is a separate, later decision.

**Wired into FORGE when a policy is active.** `forge script`, `forge rpc-serve`, `forge-serve`, model loading and
native plugin loading use the module as described under [Integration](#integration). With profile `none` they skip
it entirely, so FORGE behaves exactly as before.

## Headers

| Header | Contents | FSS |
|---|---|---|
| `errors.hpp` | `security_error` ⊃ `policy_violation`, `integrity_error`, `permission_denied`, `sandbox_error`; codes `0x5EC01`-`0x5EC04` | ERR-1, ERR-2 |
| `audit.hpp` | `audit()`, `add_audit_sink`, `set_audit_os`, `open_audit_file`, `audit_record`, `to_json` | AUD-1..3 |
| `path.hpp` | `check_relative`, `safe_join`, `try_safe_join`, `canonical_within` | CAP-2, CAP-3 |
| `policy.hpp` | `policy`, `profile`, `profile_defaults`, `parse_policy`, `load_policy`, `to_json` | Profiles, policy file |
| `enforce.hpp` | `enforce`, `try_enforce`, `capability`, `set_active_policy`, `active_policy`, `expand_root` | CAP-1 |
| `integrity.hpp` | detached signatures: `detached_sign`, `detached_verify`, `detached_verify_file`, `read_detached_sig`, `signed_name`, `detached_report`; `verify_artefact` (the `integrity.plugins` / `integrity.scripts` decision); `os_code_signature`, `code_signature` | INT-1..4 |
| `plugin.hpp` | `open_plugin` (verified native plugin load), `apply_code_signing` | INT-1..3 |
| `lockdown.hpp` | `lockdown`, `check`, `report`, `control`, `locked_down` | SBX-1..6, Verification, BLD-2 |
| `secure_buffer.hpp` | `secure_buffer` (= `os::locked_buffer`), `secure_bytes<N>`, `secure_string`, `memory_state` | CRY-2 |
| `files.hpp` | `check_private_file`, `check_private_dir`, `require_private_file`, `require_private_dir`, `file_kind` | FIO-2 |

## Example

```cpp
#include <forge/security/security.hpp>
namespace sec = forge::security;

int main(int argc, char** argv) {
    auto p = sec::load_policy(argc > 1 ? argv[1] : "");   // throws sec::policy_violation on a bad file
    auto rep = sec::lockdown(p);                           // throws sec::sandbox_error if a required control is missing
    std::fputs(rep.text().c_str(), stderr);
    sec::enforce(sec::capability::fs_write, "out/result.json");   // throws sec::permission_denied, audited
    if (auto s = sec::try_enforce(sec::capability::net_connect, "api.example.com:443"); !s) { /* s.err.what */ }
    auto path = sec::safe_join(model_dir, untrusted_name);  // CAP-3
}
```

## Policy

`forge-security.json` (shape as in [security-standard.md](security-standard.md#policy-file-planned)). Omitted fields take
the profile's values. Load order: `--policy FILE` (the argument to `load_policy`), `$FORGE_SECURITY_POLICY`,
`./forge-security.json`, `<config dir>/forge/forge-security.json` (`$XDG_CONFIG_HOME/forge`, `%APPDATA%\forge`,
`~/Library/Application Support/forge`). The first one that exists is used. A file named by `--policy` or
`$FORGE_SECURITY_POLICY` that cannot be read is an error, never a fallback.

`$FORGE_SECURITY_PROFILE` (`strict`, `standard`, `permissive`) is the base profile for a file without `"profile"`, or the
whole policy when there is no file. Using it is audited (`weaken` unless it is `strict`). A file's own `"profile"` wins.

Parsing fails closed. A syntax error, unknown key, duplicate key, wrong type or bad value throws `policy_violation`
with `file:line:col` (the position of the offending key, or of the JSON syntax error).

| Field | Values |
|---|---|
| `profile` | `strict`, `standard`, `permissive` |
| `fs.read`, `fs.write` | paths; `"*"` = anything; tokens `.` (working directory), `$TMP` (`os::temp_dir()`), `$CACHE` (`$FORGE_CACHE`, else `~/.cache/forge`), `$CONFIG` (`<config dir>/forge`), `~`. Write roots are readable too |
| `net.mode` | `none` (no sockets but `AF_UNIX`; `enforce` still allows loopback listen), `loopback`, `hosts` (loopback + `net.hosts`), `any` |
| `net.hosts` | `host`, `host:port`, `*.suffix[:port]`, `[v6]:port` |
| `exec`, `dynamic_code`, `env`, `allow_root` | booleans |
| `integrity.models/plugins/scripts` | `required`, `optional`, `off` |
| `integrity.trusted_keys` | 64-hex Ed25519 public keys |
| `limits` | `core` (null = unchanged), `nofile`, `address_space_mb`, `cpu_seconds`, `nproc` (0 = unchanged) |
| `audit` | `os` (bool), `file` (JSON-lines path) |
| `emulation` | `allow`, `deny` (a control FORGE can only emulate becomes fatal at level `required`) |

Profile defaults (`profile_defaults`):

| | strict | standard | permissive |
|---|---|---|---|
| fs.read / fs.write | `.`, `$CACHE` / `.`, `$TMP` | `*` / `.`, `$TMP` | `*` / `*` |
| net.mode | none | hosts | any |
| exec, dynamic_code, env | false | true | true |
| allow_root | false | true | true |
| models / plugins / scripts | required / required / optional | optional / required / optional | off / optional / off |
| os_code_signing | true | false | false |
| limits | core 0, nofile 1024 | core 0 | unchanged |
| audit.os | true | true | true |

## Errors

| Class | Code | `os_exception::kind` | Script class |
|---|---|---|---|
| `policy_violation` | `0x5EC01` | `other` | `PolicyViolation` (⊂ `SecurityError` ⊂ `OSError`) |
| `integrity_error` | `0x5EC02` | `other` | `IntegrityError` (⊂ `SecurityError` ⊂ `OSError`) |
| `permission_denied` | `0x5EC03` | `permission_denied` | `PermissionError` (⊂ `OSError`) |
| `sandbox_error` | `0x5EC04` | `sandbox_violation` | `SandboxViolationError` (⊂ `PermissionError`) |

All derive from `security_error`, which derives from `os::os_exception`, so existing `catch (os::os_exception&)` sites and
the interpreter's C++-to-script mapping keep working. `security_error::control` holds the FSS control ID,
`fss_name()` the FSS class name. Non-throwing calls return `os::status` whose `err.code` is one of these codes;
`security::classify(err)` maps them to `os_errc` (plain `os::classify` reports `other`), `security::throw_if(err)` throws
the matching class. The interpreter maps a `security_error` reaching a script by its code (table above);
`os_exception::script_type()` alone still reports `OSError` for the first two. `PermissionError` and
`SandboxViolationError` keep their existing place in the script hierarchy, so `catch (const SecurityError&)` does not
catch them.

## Audit

`audit(category, decision, control, target, detail)` records UTC time (ms), pid, profile, category, decision
(`allow`, `deny`, `weaken`, `fail`), control ID and target. Sinks:

- callbacks (`add_audit_sink`);
- the OS facility, `set_audit_os(true)` (done by `lockdown` when `audit.os`): `syslog(3)` (journald) on POSIX, the
  Windows event log (source `forge-security`). Called directly, so a `FORGE_LOG` level cannot hide it. Android logcat
  and Apple `os_log` go through syslog only where it exists;
- the FORGE logger, module `security` (deny / weaken at warn, fail at error, allow at info);
- a JSON-lines file, `open_audit_file(path)` (done by `lockdown` when `audit.file` is set, before the file system is
  confined): `O_APPEND | O_CREAT | O_NOFOLLOW`, mode 0600 (an existing file of this user is tightened, another user's
  is refused); on Windows a DACL for the owner and SYSTEM, and a reparse point is refused.

`audit()` never throws. A sink that throws or a short write is counted in `audit_failures()`; the decision that was
being audited does not change (AUD-3).

## Paths

`check_relative(rel)` validates an untrusted relative path with one rule set on every OS: `/` and `\` both separate;
absolute, drive, UNC and `\\?\` / `\\.\` paths, `:` (alternate data streams), NUL and control characters, Windows
device names (`CON`, `NUL`, `COM1`, `LPT¹`, `CONIN$`, ... with any extension), segments ending in `.` or space,
dot-only segments other than `.`/`..`, and `..` that would leave the root are refused. `safe_join(root, rel)` joins and
then checks the result with `canonical_within`, so a symlink inside the root that points outside is refused too
(throws `permission_denied`, control `CAP-3`). `canonical_within(path, roots)` returns the canonical path (absolute,
symlinks of the existing prefix resolved) if it lies inside a root, compared by whole components (case-insensitive on
Windows). These are checks, not handles: a link swapped in between the check and the open is outside their reach.

`modelfile::detail::safe_rel_path` (signed bundle manifests) is stricter (it refuses `\`, `.` and empty segments) and
is left as it is.

## enforce

`enforce(capability, target)` checks the active policy (`set_active_policy`, set by `lockdown`), throws
`permission_denied` and audits the denial. `try_enforce` returns `os::status`. Both have overloads taking an explicit
policy. `set_active_policy` makes the policy's `fs.read` / `fs.write` entries absolute at that moment (`.` is the
working directory then), so a later `chdir` does not move the roots.

| Capability | Allowed when |
|---|---|
| `fs_read` | canonical path under an `fs.read` or `fs.write` root, or `"*"` |
| `fs_write` | canonical path under an `fs.write` root, or `"*"` |
| `net_connect` | mode `any`; `hosts`: loopback or a `net.hosts` match; `loopback`: loopback; `none`: never |
| `net_listen` | mode `any`; otherwise a loopback address, or a `net.hosts` match in mode `hosts` |
| `exec`, `env`, `dynamic_code` | the policy flag |
| `plugin_load` | `integrity.plugins` is not `required`, or `<path>.sig` verifies against `trusted_keys` (checked on the file as it is at that moment; `open_plugin` checks the bytes it loads) |

`exec`, `dynamic_code` and `plugin_load` allowed under a non-strict profile are audited as `allow`.

## Key memory

```cpp
security::secure_buffer b(64);             // os::locked_buffer (os/locked_memory.hpp)
security::secure_bytes<32> seed;           // std::array-like: data(), size(), begin(), operator[]; clone()
security::secure_string token = "s3cret";  // converts to std::string_view; == with strings
security::memory_state(b);                 // "locked, excluded from dumps" / "unlocked (lock refused), ..."
```

Each buffer is its own anonymous mapping, rounded up to whole pages, zeroed. `secure_buffer` and `secure_bytes` are
move-only; `secure_string` is copyable (the RPC config structs are copied) and every copy is a new mapping. Destruction,
`reset()` and move-assignment over a live buffer `crypto_wipe` the whole mapping before it is unlocked and unmapped.
`os::locked_buffer::set_release_hook(fn)` lets a test see the wiped bytes before they are released.

| | Linux / Android | FreeBSD | macOS / iOS | Windows |
|---|---|---|---|---|
| lock (`locked()`) | `mlock` (RLIMIT_MEMLOCK) | `mlock` | `mlock` | `VirtualLock`; the minimum working set is raised once on `ERROR_WORKING_SET_QUOTA` |
| dump exclusion (`dump_excluded()`) | `MADV_DONTDUMP`; `MADV_WIPEONFORK` too | `MADV_NOCORE` | none: set `limits.core = 0` | `WerRegisterExcludedMemoryBlock` (WER reports only; debuggers and `MiniDumpWriteDump` still see it): the job object's die-on-unhandled-exception (`limits.core = 0`) is the process-wide answer |
| guard pages (`secure_buffer(n, true)`) | `PROT_NONE` page before and after | same | same | `PAGE_NOACCESS` |

Locking is best effort: a refused lock is reported, not an error. `check()` reports it as `CRY-2 key memory` (level
`should` in strict). Used for the Ed25519 seed in `modelfile::keypair` and `graph::rpc::identity`, RPC tokens
(`token_entry::token`, `client_security::token`), the `.fsc` signing seed and the keystore's cached store keys and
Argon2id key-encryption keys. Not covered: the TLS private key (OpenSSL's memory), the keystore passphrase string,
`model_key` (`.fpk` encryption keys), and copies a caller makes.

## Private files

`check_private_file(path, kind)` / `check_private_dir(path, kind)` return `os::status` and never follow a symlink.

| kind | POSIX | Windows |
|---|---|---|
| `secret` (default for files) | regular file / directory, not a symlink, owner = effective uid, `mode & 077 == 0` | not a reparse point, owner = the user (or Administrators for an elevated member), DACL allows only the owner, SYSTEM, Administrators (and CREATOR OWNER / OWNER RIGHTS); NULL DACL fails |
| `trusted` (default for directories) | not a symlink, owner = effective uid or root, `mode & 022 == 0` | owner = the user, SYSTEM, Administrators or TrustedInstaller; no write-type right for any other SID |

`require_private_file([policy,] what, path, kind)` applies it: profile strict throws `permission_denied` (control
`FIO-2`, audited `deny`); every other profile, `none` included, audits `weaken` (a warning in the log) and goes on. A
missing path is left to the caller's own open. Applied to: the policy file in `load_policy` (trusted; a strict policy
that fails is a `policy_violation`), `modelfile::load_secret_key` (`forge sign`), `graph::rpc::read_key_file`
(`FORGE_RPC_KEY`, `forge::remote` keys), `forge rpc-serve --token-file` / `--tls-key` (secret) and `--authorized-keys`
/ `--tls-client-map` (trusted), `FORGE_MODEL_KEY_FILE` and `forge encrypt --key` (secret). `check()` / `lockdown()`
add `FIO-2` rows for the audit file's directory (trusted) and the keystore directory and every file in it (secret,
`os::keystore::directory()`); in strict they are required, so `lockdown` refuses a group-writable audit directory or a
world-readable keystore.

`os::fs` (FIO-1): `make_temp_file(prefix, suffix, dir)` and `make_temp_dir(prefix, dir)` create exclusively, 0600 /
0700 (Windows: a protected owner + SYSTEM + Administrators DACL); `ensure_private_dir(path)` creates or tightens a
directory (an existing one must be ours and no symlink); `write_new_private_file(path, data)` writes a new 0600 file,
never an existing one, never through a symlink.

## lockdown and check

`check(policy)` reports, per control, what this process could apply; it changes nothing. `lockdown(policy)` applies
it, once per process. Both return a `report` (`json()`, `text()`, `find(name)`, `ok`). The plan is evaluated first; if a
required control would fail, `lockdown` throws `sandbox_error` before changing anything. If applying a step fails
afterwards, it throws after the fact (the steps already applied stay). Emulated, missing and failed controls are
audited (`weaken` / `fail`). With `profile::none` it does nothing.

Order: audit sinks → resource limits → no-new-privs → root check (SBX-5) → file-system confinement → syscall confinement
→ Windows mitigation policies → integrity settings → the policy becomes active for `enforce`.

States: `active`, `available`, `emulated` (FORGE's in-process `enforce()` stands in for a missing kernel mechanism),
`missing`, `n/a` (not asked for), `failed`. Levels: `required` (strict), `should` (standard), `may` (permissive),
`info`. Verdicts: missing / failed → `fail` when required, `warn` when should; emulated → `fail` only when required and
`emulation` is `deny`.

| Control | Linux / Android | Windows | macOS | FreeBSD | OpenBSD | iOS |
|---|---|---|---|---|---|---|
| limits | `setrlimit` (core, nofile, AS, CPU, NPROC) | job object (memory, CPU time, active processes, die-on-unhandled-exception); nofile n/a | `setrlimit`; AS missing | `setrlimit` | `setrlimit` | `setrlimit` |
| no-new-privs | `prctl(PR_SET_NO_NEW_PRIVS)` | n/a | n/a | `procctl` (14+) | n/a | n/a |
| SBX-5 root | `geteuid() == 0` refused unless `allow_root` | elevated token refused unless `allow_root` | same as Linux | same | same | same |
| fs write, empty list | seccomp + Landlock (`os::apply_sandbox`) | emulated | `sandbox_init` no-write | emulated | `pledge` | app sandbox |
| fs write, paths | Landlock write rules beneath each root (+ `/dev/null`); without Landlock emulated | emulated | emulated | emulated | emulated | app sandbox |
| fs read | emulated everywhere (kernel read rules would break late `dlopen`) | | | | | |
| exec denial | seccomp + Landlock | job object, one active process | `sandbox_init` pure-computation only with an empty write list, else emulated | emulated (Capsicum is all-or-nothing) | `pledge` | app sandbox |
| net `none` | seccomp (no sockets but `AF_UNIX`), Landlock TCP from ABI 4 | active inside an AppContainer without network capabilities, else emulated | `sandbox_init` no-internet | emulated | `pledge` | emulated |
| net `loopback` / `hosts` | emulated (no kernel filter by address) | emulated | emulated | emulated | emulated | emulated |
| dynamic code (W^X) | emulated | `ProcessDynamicCodePolicy` (ACG) | emulated | emulated | emulated | emulated |
| mitigations | n/a | extension points off, no remote / low-label images; strict: strict handle checks | n/a | n/a | n/a | n/a |
| MAC label (SBX-6) | `/proc/self/attr/current`, report only | not checked | not checked | not checked | not checked | not checked |
| models required / optional | `inference::model` checks the active policy (see Integration); `lockdown` also sets `FORGE_REQUIRE_SIGNED=1` and `FORGE_TRUSTED_KEYS` for `required` | same | same | same | same | same |
| scripts required / optional | detached `<script>.sig`: `engine::run_file`, imports, `rpc-serve` children (`--rpc-sig`) | same | same | same | same | same |
| threads | seccomp `TSYNC` covers threads started before `lockdown`; Landlock write rules only the calling thread and later ones | job object / mitigations: whole process | whole process | n/a | whole process | n/a |
| plugins required / optional | detached `<plugin>.sig`, `open_plugin`; the verified bytes are loaded from a memfd | same; the file is held without write / delete sharing until `LoadLibraryExW` | same; `dlopen` with an identity check | as macOS | as macOS | as macOS |
| INT-3 OS code signing | n/a (no mechanism for shared objects; `security.ima` presence reported only): `missing`, level `info` | `WinVerifyTrust` (embedded, then catalog) | `SecStaticCodeCheckValidity` | n/a | n/a | n/a |
| BLD-1 hardening | `forge::build_hardening()` when `core/build_info.hpp` exists, else the compiler's macros (`__SSP__`, `_FORTIFY_SOURCE`, `__PIE__`, `__CET__`) | same (MSVC: `/GS`) | same | same | same | same |

Also reported: CAP-1/CAP-2 (in-process, `info`), CRY-1 (OS CSPRNG probe), CRY-2 (key memory lock state), FIO-1 (temp
files, `info`), FIO-2 (audit directory and keystore, see [Private files](#private-files)), AUD-1 (OS facility, audit file).

`forge security check [--policy F] [--json]` prints `check()` for `load_policy()` and exits 1 when `ok` is false (see
[cli.md](cli.md)). A strict policy with `"emulation": "deny"` fails on every OS today, because file reads and
W^X are only emulated.

## Integration

Every FORGE program below loads its policy with `load_policy(--policy)` and does nothing more when the result is
profile `none` (no policy file, no `FORGE_SECURITY_PROFILE`): output and behaviour are those of a build without the
module. With an active policy:

| Where | What happens | FSS |
|---|---|---|
| `forge script [--policy F]` | `integrity.scripts` `required` refuses `-e` code and the REPL (no file, no signature; `integrity_error`, audited). Capabilities from the policy: `net` iff `net.mode` ≠ `none`, `proc` iff `exec`, `env` iff `env`, `fs` unless both root lists are empty; `--no-*` flags narrow the policy. The decision is audited, then `lockdown(policy)` runs before any script code (it replaces `--os-sandbox`) | P1, INT-1, SBX-1 |
| script built-ins (any host) | the runtime hook `enforce_scope(Interp&, SecScope, target)` (`script/src/interp.hpp`) calls `try_enforce` against `active_policy()` with the path (`fs_read` / `fs_write`), `host:port` (`net_connect`), program (`exec`), variable (`env`) or plugin path (`plugin_load`) and raises `PermissionError`. It runs after the `Caps` flag check and is a no-op under profile `none`. Embedders that call `lockdown` / `set_active_policy` get it too | CAP-1 |
| script files (`engine::run_file`, `import`, `#include`) | `verify_artefact(active_policy(), script, path, bytes)` on the buffer that is parsed: `integrity.scripts` `required` needs `<file>.sig` by a trusted key (`IntegrityError` otherwise, audited `INT-4`); `optional` verifies signed files, runs unsigned ones (audited `weaken`) | INT-1, INT-2 |
| `rpc-serve` remote scripts | the client sends the script's `.sig` text with the source (`FRPZ` frame; `forge::remote(addr, src, {{"name", "job.fs"}, {"signature", sig}})`, `client::run_script(..., signature)`); the server passes it to the child (`--rpc-sig <hex>`), which verifies the source it read from stdin against the script name before running it | INT-1, INT-2 |
| native plugins (`forge::plugin`, `import "x.so"`) | `fs_read` on the path, then `security::open_plugin`: `plugin_load`; `integrity.plugins` on the bytes read through one handle (`<plugin>.sig`); `integrity.os_code_signing`: `os_code_signature` must be `signed` (Windows, macOS; `n/a` elsewhere is audited `weaken`). The verified bytes are what gets mapped: Windows holds the file open without `FILE_SHARE_WRITE` / `FILE_SHARE_DELETE` until `LoadLibraryExW` returns (a file someone has open for writing is refused); Linux / Android `dlopen` a memfd copy of the verified buffer; others `dlopen` the path and compare device, inode, size, mtime and ctime with the file that was read, before and after (residual race: an in-place rewrite within one ctime tick). Failures: `integrity_error` (`IntegrityError`), audited `INT-4` / `INT-3` | INT-1..4, P4 |
| `inference::model` (`forge run`, `forge bench`, `forge-serve`, `forge::ask`, the C API) | `fs_read` on the path before the file is opened. `integrity.models` `required`: BLAKE3 hashes, then the embedded Ed25519 signature against `trusted_keys` (any valid signature when empty) before the blueprint or a tensor is read; `optional`: signed files verified, unsigned audited (`weaken`). Failure: `integrity_error`, audited `INT-4`. `FORGE_REQUIRE_SIGNED` / `FORGE_TRUSTED_KEYS` keep working and add keys | INT-1, INT-4 |
| `forge run`, `forge bench` | `set_active_policy(policy)` (no lockdown) so model loads follow it | INT-1 |
| `forge import`, `forge sign`, `forge encrypt --key` | `set_active_policy(policy)`; importers check `fs_read` on every input and `fs_write` on the output; the secret key file must pass FIO-2 | CAP-1, FIO-2 |
| `modelfile::reader` (every `.fpk` opened) | `fs_read` on the path | CAP-1 |
| importers (`import::safetensors`, `gguf_file`, `import_hf`, `import_gguf`, `import_onnx`) | `fs_read` on inputs (`config.json`, each `.safetensors`, the `.gguf` / `.onnx`), `fs_write` on the output | CAP-1 |
| `.fsc` script cache (`fsc::load` / `fsc::store`) | `fs_read` / `fs_write` on the cache directory; a denial is audited and the source is parsed as on a miss | CAP-1 |
| KV spill (`mem::spill_store`, `forge-serve --kv-spill`) | `fs_write` on the directory through `os::check_access` (the gate `set_active_policy` installs; core cannot include `forge::security`); a denial throws. Always: a private directory (a fresh random one by default), 0600 files created exclusively | CAP-1, FIO-1 |
| key and credential files | see [Private files](#private-files) | FIO-2 |
| `forge rpc-serve` | `net_listen` on `--bind:--port`; `--allow-scripts` needs `exec` (`policy_violation` otherwise); `lockdown` after the listener is bound, before `run()`; children get `--policy <abs>` | SBX-1 |
| `forge-serve` | policy active before the model loads; `net_listen` on `--host:--port`; `--watch` needs `exec`; `lockdown` after bind + load, before the first `accept` | SBX-1 |
| `forge-serve proxy` | `net_listen`, `net_connect` for every backend; `lockdown` after the listener starts | SBX-1 |

The servers print every control whose verdict is not `ok` to stderr and audit `SBX-1` with the listen address.
`lockdown` on Linux also installs its seccomp filter with `SECCOMP_FILTER_FLAG_TSYNC`, so threads a server started
before locking down (scheduler, pools) are filtered too; Landlock has no such flag on the kernels FORGE targets, so
its write rules bind only the locking thread and threads started afterwards (the `fs: write confinement` detail says
so). `forge security check` is unchanged.

## Tests

`tests/test_security.cpp` (ctest `test_security`, label `security`): profile defaults, parsing (full document,
omitted fields, unknown and duplicate keys, wrong types, `line:col`), the load order and environment variables,
attack strings for `check_relative` / `safe_join` / `canonical_within` (POSIX and Windows forms, symlink escapes),
`enforce` decisions and audit records through a callback sink, a throwing sink, audit file mode and symlink refusal,
`check()` JSON shape, and three child processes (the binary re-runs itself with `FORGE_SECURITY_TEST_CHILD`):
`lockdown` (writes outside the write root fail under Landlock, exec fails, core and nofile limits, no-new-privs, ACG on
Windows, the audit file), `refuse` (strict with `emulation: deny` is refused before anything is applied) and `models`.
Run on Linux (WSL2, kernel Landlock ABI 3; also with `FORGE_LANDLOCK=0`) and Windows (MinGW). macOS, FreeBSD and MSVC:
CI only.

Also in `test_security`: `secure_buffer` (zeroed, wiped before release, observed through the release hook; move and
move-assignment wipe; lock state and `MADV_DONTDUMP` reported; guard pages; `secure_bytes` / `secure_string`; the keys
loaded by `modelfile` and rpc), private temp files and directories (exclusive, no symlink), `check_private_file` (POSIX:
0644 fails, 0600 passes, group-writable trusted fails, symlink fails, other owner only as root; Windows: a DACL with
Everyone fails), a strict policy refusing a world-readable key (`require_private_file`, `load_secret_key`,
`read_key_file`) and a group/other-writable strict policy file, and importers, the `.fpk` reader and the KV spill
store under a strict policy (denied outside the roots, private inside). `test_script_fsc` checks the `.fsc` cache under
a strict policy.

`tests/test_script_security.cpp` (ctest `test_script_security`): no policy is profile `none` and audits nothing; every
`tests/script/*.fs` golden gives the same output with no policy and with an active `permissive` policy; a strict
policy confined to a directory allows script file operations inside it and refuses writes, reads, listings, copies,
renames, `..` escapes and globs outside it (audited), refuses net / exec / env even with the `Caps` flags on; mode
`hosts` allows listed hosts only; the `SecurityError` script classes; plugins under `integrity.plugins` optional /
required (unsigned: `IntegrityError`; the `plugin_load` gate alone also denies); model loads under `integrity.models` required (signed by the trusted key: loads; unsigned, tampered or
signed by another key: `integrity_error`, audited) and optional, and `fs_read` on the model path; a child process
that runs `lockdown` and then a script. `tests/security_cli_test.py` (ctest `security_cli`, needs the tiny golden
model): `forge run` / `forge script` give the same output with no policy and a permissive one; under a strict policy
`forge run` needs the signed model, `forge script` is confined (fs scoped, no net / proc / env, lockdown audited) and
`integrity.scripts: required` refuses `-e` code; `rpc-serve` locks down and accepts loopback connections, refuses
`--allow-scripts` without exec and a non-loopback bind, and with exec granted runs a remote script confined to the
policy; `forge-serve` serves the signed model on loopback with the same tokens and refuses an unsigned model and a
non-loopback host.

`tests/test_signing_detached.cpp` (ctest `test_signing_detached`, label `security`): the detached format (round trip,
tampered content, wrong key, another file name, a forged `file` field, flipped signature bytes, malformed and unsigned
`.sig`, a second signer, two signed files swapped), `verify_artefact` under `required` / `optional` with the audit
records, a script and its import under `integrity.scripts` (unsigned, untrusted key, modified after signing), the
example plugin (`forge_script_example_plugin`) under `integrity.plugins` (unsigned, signed, other key, renamed,
tampered, optional, `os_code_signing`; on Windows a file held open for writing is refused) and `os_code_signature`
(Windows: `notepad.exe` is catalog-signed by Microsoft Windows, `MpSigStub.exe` has an embedded signature, the test
binary is unsigned; macOS: `/bin/ls` signed, the test binary unsigned or ad-hoc; Linux: `n/a`).
`tests/signing_cli_test.py` (ctest `signing_cli`, no model or Python modules needed): `forge sign` / `forge verify`
on a script (wrong key, a second signer, another name, unsigned, tampered), `forge script FILE` under
`integrity.scripts` required / optional with an import, and `rpc-serve` running a signed remote script and refusing
an unsigned one, one signed for another name and a modified one. Both run on Linux (WSL2) and natively on Windows
(MinGW); macOS (`SecStaticCode`), FreeBSD and MSVC: CI only.
