# Docs claims audit (historical record)

This is a record of past audits of documentation claims against the code. It is not a description of the
current code. For current behaviour read the topic docs; for test results read [STATUS.md](STATUS.md).

## 2026-11-04: full docs rewrite

All docs were rewritten and re-checked against the source at main ccc0a7d. Corrections made in the
security and status docs:

| Old claim | Now |
|---|---|
| No `.fsc` script cache (dropped) | It exists (`script/src/fsc.cpp`, `--cache-dir`); it has no tests. |
| Remote scripts run one at a time | Each runs in its own child process (`forge script --rpc-child`) with timeout and memory cap. |
| FSS v1 (security-standard.md) is normative | Marked as a draft: `forge::security`, the policy file, profiles, `forge security check` and `FORGE_HARDENING` do not exist. Each control now states what exists today. |
| `run_sandboxed` applies the whole policy | On POSIX it drops `deny_exec` for the target so `execvp` can run. |
| OS sandbox run-tested on Linux only | `test_os_ext` sandboxing runs in CI on Linux, macOS, Windows and FreeBSD. |
| macOS only cross-linked; iOS never built; Windows event log sink and dynlib Windows/Apple code never compiled | All built in CI; `test_log` and `test_os_dynlib` pass on macOS and Windows. iOS is build-only. |
| CUDA/HIP/OpenCL/Level Zero/Metal never on hardware | CUDA ran on an RTX 3090; Metal passes on the GitHub macOS runner. HIP, OpenCL, Level Zero still mock-only. |
| CI workflow must be moved by hand | It is at `.github/workflows/ci.yml`. |
| Keystore fallback (implied encrypted) | The file fallback is XOR-padded, not encrypted. |

## 2026-10-31: audit on branch docs-verified (base 5940593), merged in PR #12

Scope: scripting.md, security claims in rpc.md/modelfile.md/os.md, index.md, TODO.md, STATUS.md headlines,
every backticked file path, and `forge script` CLI options. Other docs were not checked line by line.

False claims found and fixed then:

| Claim | Finding then | Status 2026-11-04 |
|---|---|---|
| Scripts sandboxed "using sandbox.hpp" | No OS sandbox in script/src | Now true: `engine.cpp` applies it for `--no-fs/--no-net/--no-proc` (PR #27). |
| `.fsc` bytecode cache | Absent | Now exists. |
| `--mem-limit` for scripts | Absent | Still absent; `forge script` has `--timeout`, `--max-steps`, `--heap`. |
| `limit {mem: "2G", ...}` block syntax | Absent | Now exists (`limit {...} { body }`, size/duration strings). |
| Recursion depth default 10 000 | It was 800 | See `script/src/interp.hpp` `Limits`. |
| Allocation, handle and child-count limits | Absent | Now exist (`allocs`, `handles`, `procs`). |
| `tests/script/*.fs` goldens | Absent | Added in PR #11. |
| Script parser fuzz target | Absent | Added in PR #11 (`fuzz/fuzz_script.cpp`). |
| `dynlib.hpp` module | Absent | Now exists (`os/include/forge/os/dynlib.hpp`). |
| Scripting is "design, not yet built" | It was built | — |
| No C++ exception escapes the VM | Partly true; the engine catches all, the VM does not | Wording kept as "engine catches all". |

A second pass re-derived the changed sentences from source and found 5 more errors (an example map literal,
lazy REPL compilation wrongly marked false, goldens, fuzz target, `forge::limit` test). Claims verified then
included: capability checks raise `PermissionError`; AI code needs confirmation; plugins are refused in
sandboxed runs; remote scripts need auth; `.fpk` Ed25519 signing.
