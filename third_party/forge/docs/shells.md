# Shell backends

Every shell is a backend: one file in `script/src/shells/` that defines
`forge_shell_register_<file>(forge::script::shell_registry&)`. CMake globs that directory and generates the
registry, so adding a shell takes one file. The interface is `script/src/shells/shell.hpp`.

For each backend name and alias, ForgeScript has `forge::<name>(template, args...)`. It replaces each `{}` with the
next argument quoted for that shell, runs the first executable it finds, and returns a `ProcResult` (`out`, `err`,
`code`, `ok`, `ms`, `timed_out`, `lines()`, `json()`, `check()`). A missing shell raises `ShellError`. See
[scripting.md](scripting.md#shell-calls).

- `forge::shell(tmpl, args...)` runs through `forge::host_shell()`: `$FORGE_SHELL` if set; on Windows `pwsh`, then
  `powershell`, then `cmd`; on Android `mksh` if a binary of that name exists, else `sh`; `sh` elsewhere.
- `forge::shells()` lists the backends as `{name, family, available, exe, tested, emit, emit_tested, aliases}`.
- A backend may provide an emitter, used by `forge script --emit` (see
  [scripting.md](scripting.md#write-once-emit-anywhere)).

**Finding the executable.** Each backend lists executables to try in order. On Windows every `PATH` entry is
searched and the full path is used; the WSL `bash.exe` relays (in `System32` or `WindowsApps`) are not accepted as
bash, and the Windows Kits Resource Compiler is not accepted as `rc`. On POSIX a match ending in `.exe` (a Windows
program reached through WSL interop) is skipped.

| backend | aliases | executables | invocation | `tested` | emit ext |
|---|---|---|---|---|---|
| sh | | `sh` | `-c` | yes | `.sh` |
| bash, zsh, dash, yash, hush | | same name | `-c` | yes | `.sh` |
| ash | busybox | `ash`, `busybox` | `-c`; `busybox sh -c` via busybox | yes | `.sh` |
| ksh | ksh93 | `ksh`, `ksh93` | `-c` | yes | `.sh` |
| mksh | android, toybox | `mksh` | `-c` | yes | `.sh` |
| oksh | pdksh | `oksh`, `pdksh` | `-c` | yes | `.sh` |
| posh | | `posh` | `-c` | **no** | `.sh` |
| osh | oils | `osh` | `-c` | yes | `.sh` |
| ysh | | `ysh` | `-c`; J8 `b'…'` string literals | yes | `.ysh` |
| fish | | `fish` | `-c` | yes | `.fish` |
| tcsh | | `tcsh` | `-f -c` | yes | `.csh` |
| csh | | `csh`, then `tcsh` | `-f -c` | **no** (real BSD csh) | `.csh` |
| nu | nushell | `nu` | `--no-config-file -c`, raw strings `r#'…'#` | yes | `.nu` |
| xonsh | | `xonsh` | `--no-rc -c`, Python literals | yes | `.xsh` |
| elvish | | `elvish` | `-norc -c` | yes | `.elv` |
| pwsh (PowerShell 7) | ps | `pwsh`, `powershell` | `-NoProfile -NonInteractive -Command` | yes | `.ps1` |
| powershell (Windows PowerShell 5.1) | powershell5, winps | `powershell`, `pwsh` | same as pwsh | **no** | `.ps1` |
| cmd | | `cmd.exe`, `cmd` (only `cmd.exe` on Android) | `/d /s /c`, `"` doubled, `%` as `%%` | **no** | `.cmd` |
| rc, es | | same name | `-c`, `'` doubled | yes | `.rc`, `.es` |

`tested` is the backend's `golden_tested` flag in code. The emitter's own flag (`emit_tested`) is the same except
that it is false for powershell and cmd. Off Windows, `powershell` falls back to `pwsh`.

**Tests** (`tests/test_script.cpp`):

- `shell_backends_golden` round-trips a hostile literal through `{}` quoting in each available shell and skips
  missing ones. cmd is skipped there (`echo` keeps the quotes); the Windows CI job covers it through the emit goldens.
- `emit_golden_all_shells` and `emit_text_golden` cover the emitters; see
  [scripting.md](scripting.md#write-once-emit-anywhere).
- ctest adds `FORGE_TEST_SHELL_PATH` (CMake cache variable; default `../opt/shells/bin` when that directory exists)
  to `PATH`, so shells installed outside the system paths are found.

Which shells a given machine or CI job actually has installed decides which of these run; the `tested` flag says
that the backend is expected to pass where the shell exists.
