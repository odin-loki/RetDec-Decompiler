# forge::os audit (2026-11-05)

A read-only audit of every `forge::os` header and the compiled `os/src` backends (mobile, text, locale), branch by
branch on Linux, Android, Windows (MSVC, MinGW), macOS, iOS and FreeBSD. Categories: **C** bug or security risk,
**B** partial or degraded behaviour, **A** unsupported where the OS could support it, **D** untested path,
**E** inherent limit (documented, not fixed). Status: open, fixed (commit), or won't fix (reason).

The work is split by file ownership so changes can proceed in parallel without conflicts.

## Wave 1: bugs, security, degraded behaviour

### W1-A: processes, sandbox, signals, pipes, crash (`process.hpp`, `process_pool.hpp`, `sandbox.hpp`, `signal.hpp`, `pipe.hpp`, `crash.hpp`)
| ID | Cat | Item | Status |
|---|---|---|---|
| A1 | C | `send_signal`: pid 0 signals our own group; pid ≤ 1 with `group` becomes `kill(-1)`; pid truncation | fixed: pid 0, pid above pid_t / DWORD and `group` with pid ≤ 1 rejected (EINVAL); `kill_group` range-checked too |
| A2 | C | Pipes from `pipe()` without atomic close-on-exec (spawn, child, process_pool, pipe.hpp): concurrent forks inherit them, readers never see EOF | fixed: `detail::pipe_cloexec` (pipe2 `O_CLOEXEC` on Linux/Android/FreeBSD; Apple: pipe + `FD_CLOEXEC` under `fork_mutex`) everywhere; children move ends onto 0/1/2 with `dup_to` |
| A3 | C | Windows `spawn_wait`: stdin written synchronously (deadlock, timeout ignored); readers wait forever on a grandchild holding the pipe | fixed: stdin writer thread; after the direct child exits, 200 ms grace, then threads drain only buffered data and `CancelSynchronousIo` ends blocked I/O; handles created non-inheritable |
| A4 | C | Windows `.bat`/`.cmd` arguments through `cmd /c` with MSVCRT quoting (BatBadBut injection) | fixed: Rust std's scheme (`cmd.exe /e:ON /v:OFF /d /c ""script" args"`, quote unsafe ASCII, double `"`, `%` → `%%cd:~,%`), cmd.exe from the system directory; CR/LF/NUL arguments and quoted script names refused |
| A5 | B | Windows environment block is case-sensitive (`Path` vs `PATH` duplicates) | fixed: names compared with `CompareStringOrdinal(…, TRUE)`, existing casing kept |
| A6 | B | Windows job: `no_core` ignored; failed `AssignProcessToJobObject` silently drops limits and tree kill | fixed: `no_core` → `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION` (not `SEM_NOGPFAULTERRORBOX`: another process's error mode can only be set by racing our process-wide one); job create / limit / assign failure terminates the child and returns an error; `open_files` / `file_size_bytes` documented POSIX-only |
| A7 | C | Windows process_pool: CRT fd 1 not redirected (task `printf` corrupts IPC); `reap` orphans on timeout; few NTSTATUS codes mapped | fixed: worker duplicates the IPC handles, CRT fd 1 → stderr (NUL without one), fd 0 → NUL; a worker not gone 5 s after losing its pipes is terminated (kind timed_out); NTSTATUS map adds in-page, misalignment, float, privileged, overflow, stack overflow, bounds, fast fail, heap corruption, invalid CRT parameter |
| A8 | C | process_pool sandbox on Windows: unsupported policy swallowed and recorded as applied | fixed: any `apply_sandbox` error (unsupported included) exits the worker with 70 and fails the task (kind io, "sandbox could not be applied"); `note_sandbox_policy` only after success. AppContainer workers: W2-3 |
| A9 | C | seccomp: io_uring, x32 and i386 `socketcall` bypasses; `linkat`/`symlinkat`/`mknodat`/`fchmodat`/`fchownat`/`utimensat`/`*setxattr` not denied | fixed: io_uring setup/enter/register denied with network or write denial; x32 (nr ≥ 0x40000000) kills; `socketcall` denied; link/symlink/mknod/chmod/fchmodat2/chown/utimes/xattr calls (non-at, 32-bit uid, time64, truncate64 variants too) denied; `run_sandboxed` builds the filter before fork |
| A10 | C | FreeBSD `run_sandboxed`: `cap_enter` before `execvp` makes every child exit 127 | fixed: binary, PT_INTERP linker and library dirs opened before fork; child `cap_enter()` then `fexecve` (static) or `ld-elf.so.1 -f <fd>` with `LD_LIBRARY_PATH_FDS` (dynamic); scripts refused (ENOEXEC). Verified on FreeBSD 14.5 |
| A11 | C | Windows `run_sandboxed`: arguments quoted without escaping | fixed: `detail_proc::win_cmdline` (batch handling included), resolved exe as application name |
| A12 | C | Crash handler: non-async-signal-safe calls, predictable `/tmp` report path without `O_EXCL`/`O_NOFOLLOW`, ANSI paths on Windows, altstack only on the installing thread, no chaining, CRT abort paths bypass it | fixed: handler formats with `detail::sig_text` (no stdio/malloc); unwinder and dladdr warmed at install (not formally AS-safe: a crash inside the loader can still hang); default dir `<temp>/forge-crash-<euid>` 0700, owner/symlink checked; `O_CREAT\|O_EXCL\|O_NOFOLLOW` with -1..-9 suffixes, pid taken at crash time; `after` is a plain function pointer; previous sigaction chained; `install_thread_stack()` for other threads; Windows wide paths, previous SEH filter chained, SIGABRT / invalid-parameter / purecall handlers |
| A13 | D | Tests: `run_sandboxed` everywhere, pool sandbox, large stdin, grandchild, concurrent `child::start` | fixed: run_sandboxed (Linux, FreeBSD, Windows incl. argv quoting and AppContainer), Windows pool sandbox failure and task stdio, 1 MB stdin with timeout and early exit, background grandchild, concurrent `child::start` / `spawn_wait` to EOF, invalid `send_signal` pids, batch `"&echo pwned&"` and `%PATH%`, case-insensitive PATH, crash chaining / abort / private dir |

### W1-B: core, files, IPC, console, time (`os.hpp`, `oserror.hpp`, `async_file.hpp`, `local_ipc.hpp`, `shared_memory.hpp`, `fs.hpp`, `console.hpp`, `time.hpp`, `detail.hpp`)
| ID | Cat | Item | Status |
|---|---|---|---|
| B1 | C | Windows `local_listener::accept` timeout: `CancelIo` without waiting, kernel can complete into a dead stack `OVERLAPPED` | fixed (b00d79e): CancelIoEx + blocking GetOverlappedResult, wait failure reported |
| B2 | C | `local_ipc`: client lacks `SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION`; `listen` unlinks any file at the path; no peer-credential check | fixed (b00d79e): SQOS identification, S_ISSOCK-only unlink, `local_stream::peer()` |
| B3 | C | Windows IOCP: `post_error_` drops the error; handle-value cache misroutes I/O after handle reuse; reserved key collides with user keys | fixed (fdd96be): errors as the key's completion, file-identity cache with exact access, internal keys, `forget()` |
| B4 | C | Close-on-exec races: `run()` pipe, `socket::make`, `socket::accept`, local_ipc sockets; Windows sockets inheritable; `SO_REUSEADDR` port hijack on Windows | fixed (fdd96be, b00d79e): SOCK_CLOEXEC/pipe2/accept4, Apple under `fork_mutex` (accept window documented), NO_HANDLE_INHERIT, SO_EXCLUSIVEADDRUSE |
| B5 | C | Windows `fs`: byte-wise widening of paths (`file_lock`, `write_file_atomic`, `make_temp_file`); `write_file_atomic` not durable, ignores mode; `make_temp_file` not exclusive | fixed (30de8e8) |
| B6 | C | Windows `stat`: every reparse point (OneDrive placeholders, dedup) reported as a link; target size/mtime not followed | fixed (fdd96be): only SYMLINK/MOUNT_POINT tags, followed via handle |
| B7 | C | io_uring: no opcode probe (kernels 5.1–5.5), tail not rolled back on failure, length truncation | fixed (fdd96be) |
| B8 | C | `shared_memory`: Apple name truncation collisions, `O_TRUNC` on existing regions, name leaks on error, Windows existing mapping not detected, Android default dir | fixed (b00d79e); Windows `open(n=0)` page-rounded size documented |
| B9 | B | `classify` on Windows treats errno/`EAI_*` codes as Win32; missing Win32 mappings | fixed (fdd96be): `err_domain` on `error`, `classify_gai` |
| B10 | B | Smaller `os.hpp` items: append emulation, DWORD truncation, `pread` moves the pointer, `map_file` offset checks, `prot` ignored, `temp_dir` overflow, thread priority, `pin_thread` shift UB and FreeBSD support, `thread` move-assign leak, `socket::connect` error, `sleep_ns` granularity | fixed (fdd96be); Windows `pread` file position documented |
| B11 | B | macOS `available_memory` is total/4 (use `host_statistics64`); FreeBSD free-only count | fixed (fdd96be) |
| B12 | B | Console on Windows: `_isatty`, size fallback, VT input, `read_password` Unicode; OSC 8 links emitted without colour and unsanitised | fixed (30de8e8); OSC 8 lives in `os/src/text/console.cpp` (W1-E) |
| B13 | A | `timezone_name` on Windows, Android, FreeBSD; `on_signal(hangup)` on Windows | fixed (fdd96be, 30de8e8) |
| B14 | A | `pipe.hpp` on Windows | fixed (30de8e8) |
| B15 | B | Linux `mounts` unescape can throw; `localtime_s` unchecked; `read_some` EINTR as EOF | fixed (30de8e8) |
| B16 | D | Tests: non-ASCII paths, classify tables, IOCP errors, handle reuse, shm collisions, timezone, console | fixed (335fb56); Linux, Windows (MinGW, native), FreeBSD; macOS/MSVC CI only |

### W1-C: system, hardware, network, watching (`net.hpp`, `sysinfo.hpp`, `cpu_topology.hpp`, `numa.hpp`, `hugepages.hpp`, `memory_pressure.hpp`, `power.hpp`, `perf.hpp`, `gpu_devices.hpp`, `devices.hpp`, `clipboard.hpp`, `dynlib.hpp`, `file_watch.hpp`, `file_watch_tree.hpp`)
| ID | Cat | Item | Status |
|---|---|---|---|
| C1 | C | `net::http`: CR/LF/NUL injection in method, path, headers; credentials copied across redirect origins; IPv6 `Host`; relative redirects | fixed 2702fd4: CR/LF/NUL rejected, credential headers dropped across origins, bracketed IPv6, RFC 3986 `resolve_reference` |
| C2 | C | `net::http`: truncated bodies returned as success; no overall deadline; repeated `Set-Cookie` merged | fixed 2702fd4, 5038e80: short/unterminated/bad-chunk bodies are errors; `deadline_ms` (default 5 min); `header_list` keeps repeats |
| C3 | C | `CPU_SET` without bounds (`pin_thread_to`, `bind_thread_to_node`) on Linux and FreeBSD | fixed 2bbe188: bounds-checked (dynamic sets for large ids) |
| C4 | C | `memory_pressure`: dead monitor after stop/subscribe; Windows event leak; Apple dispatch source never cancelled | fixed 3f4a547: pipe drained, event reused, dispatch source cancelled; `monitoring()` |
| C5 | C | Windows clipboard reads unterminated foreign data; no retry on contention | fixed 3b6241b: bounded by `GlobalSize`, `OpenClipboard` retried |
| C6 | C | System DLLs loaded through the default search order (use `LoadLibraryExW` + `LOAD_LIBRARY_SEARCH_SYSTEM32`) | fixed 2702fd4, 3b6241b; crash, users and mobile services in the merge (`os.hpp` `dynlib::open` keeps the default order for caller-named libraries) |
| C7 | C | FreeBSD partition prefix match; `gpu_devices` PCI batch; Windows `current_cpu` group-relative; hugepages privilege race; perf timer handle leak | fixed 3b6241b, 2bbe188 |
| C8 | C | `file_watch`: >64 Windows directories silently dead; overflow dropped; case-sensitive names on Windows; kqueue fd exhaustion; tree watcher symlink loops | fixed 63b7840: one IOCP, `overflow` + rescan, case-insensitive, kqueue cap → polling, loop detection by file id |
| C9 | B | MSVC: no rdtsc/cpuid (`cycle_counter` in ns, empty CPU brand) | fixed 2bbe188 (`__rdtsc`/`__cpuid`; MSVC ARM64 still ns) |
| C10 | B | `sysinfo::memory`: swap 0 on macOS/FreeBSD, Windows free = available, Linux root cgroup only; Windows `self_usage` peak RSS/faults; Windows 11 version | fixed 3f4a547 |
| C11 | B | `gpu_devices` integrated/VRAM heuristics; `power` thermal/throttle/charging/multi-battery heuristics; perf hybrid PMUs and multiplexing | fixed 3b6241b, 2bbe188; AMD APU and MhzLimit checks stay heuristics |
| C12 | B | Clipboard: macOS UTF-8, X11 empty selection; `dlvsym` on FreeBSD/bionic; Linux PSI in containers | fixed 3b6241b, 3f4a547 |
| C13 | A | perf: macOS `proc_pid_rusage` cycles/instructions, FreeBSD libpmc; numa on Windows/FreeBSD; hugepages Windows 1G and FreeBSD largepage shm | fixed 2bbe188; per-node `total_bytes` 0 on multi-node Windows/FreeBSD (no API) |
| C14 | D | Tests for each of the above | fixed (tests in each commit) |

### W1-D: services, keystore, users, settings, registry, background (`keystore.hpp`, `service.hpp`, `service_control.hpp`, `background.hpp`, `users.hpp`, `registry.hpp`, `settings.hpp`)
| ID | Cat | Item |
|---|---|---|
| D1 | C | Keystore file fallback is XOR obfuscation with no integrity; pad races; permissions set after writing; chmod of any dir; replace with XChaCha20-Poly1305 (monocypher, already in `third_party/`) and a wrapped key — **fixed** (0b76e23): XChaCha20-Poly1305 per secret, exclusive 0600 store key (DPAPI-wrapped on Windows), XOR files migrated; key wrapping is W2-4 |
| D2 | B | Keystore: libsecret failures silently fall back to the file while reporting "libsecret"; FreeBSD never tries libsecret; Android default dir unusable; Apple put deletes before add — **fixed** (0b76e23) |
| D3 | C | `service::log`: `openlog` keeps a pointer into a string that is reassigned on every call — **fixed** (d3ec150) |
| D4 | C | systemd unit rendering: newline injection, `$` doubling, unquoted `;` — **fixed** (d3ec150): `validate()` rejects control characters; escaping per systemd.syntax/exec |
| D5 | B | macOS service `query` reports loaded-but-stopped as running; deprecated `load`/`unload` — **fixed** (d3ec150): `bootstrap`/`kickstart`/`bootout gui/<uid>`, state from `launchctl print` (macOS CI only) |
| D6 | B | `background`: Windows per-thread `SetThreadExecutionState`; iOS nil expiration handler; Linux/FreeBSD no inhibitor — **fixed** (86d413e): power requests, iOS expiration block, logind `sleep:idle` inhibitor; FreeBSD has no idle-suspend policy (documented) |
| D7 | B | `user_directory` ignores Known Folders (OneDrive redirection) and `user-dirs.dirs`; Windows `current_user` groups/full name — **fixed** (97badb0): `SHGetKnownFolderPath`, `user-dirs.dirs`, token groups + full name |
| D8 | B | Registry: no WOW64 view, type changes on write, no MULTI_SZ/BINARY — **fixed** (01d2232): `view`, type-preserving `set`, `set_as`/`value_type`, MULTI_SZ/BINARY |
| D9 | B | `settings`: ANSI `getenv` on Windows; `strtoll` base 0 parses "010" as octal — **fixed** (4d87ece): UTF-16 environment, decimal or `0x` integers, range checks |
| D10 | D | Tests: backends that silently pass when `put` fails, services, registry, user_directory, settings — **fixed** (all of the above): keystore must work + tamper/swap/migration/6-process race, unit injection/escaping, `launchctl print` parsing, registry types/list/views, Known Folders vs the shell, `user-dirs.dirs`, settings `010`/non-ASCII; Linux, Windows (MinGW), FreeBSD |

### W1-E: mobile, text, locale (`os/src/mobile`, `os/src/text`, `mobile.hpp`, `text.hpp`, `locale.hpp`)
| ID | Cat | Item |
|---|---|---|
| E1 | C | `zone_at` opens caller-supplied absolute paths with no type or size check; unbounded cache — **fixed (6831dbf): IANA-style names only, regular files ≤ 1 MiB opened non-blocking, absolute paths only from `$TZ`, 64-entry cache of successes** |
| E2 | C | `open_url` accepts any string containing `://` (local executables, option injection) — **fixed (c4f1ea2): RFC 3986 scheme at position 0 on an allow-list (`set_url_schemes`, default http/https/mailto/tel), no controls/spaces, checked before hook and opener** |
| E3 | C | gettext plural expression `!` recursion unbounded — **fixed (bd3c9bd)** |
| E4 | C | Windows pointer timestamps in QPC ticks; iOS shared `CMMotionManager` across streams — **fixed (314202b, c4f1ea2): OSC 8 dropped at `none` and sanitised; QPC → ns, GetPointerFrameInfo count; shared CMMotionManager with per-kind fan-out (iOS compiled in CI only)** |
| E5 | B | Sensors with no native source cannot use the bridge feed — **fixed (c4f1ea2): `provide_sensor(kind)` gives a bridge-only stream on any backend without a native source** |
| E6 | B | Linux/FreeBSD touch picks touchpads, raw units, REALTIME clock — **fixed (c4f1ea2): INPUT_PROP_DIRECT, EVIOCGABS → display pixels, EVIOCSCLOCKID monotonic, every matching node** |
| E7 | B | Windows keep_awake per thread, device info; Android rotation vector `w`, unchecked `registerSensor` — **fixed (c4f1ea2): PowerCreateRequest/PowerSetRequest, SMBIOS registry info; rotation `w`, registerSensor errors returned** |
| E8 | B | Linux cameras/torch/brightness/biometrics/form factor/bluetooth/nmcli parsing; services paths on Android/Windows/Linux; audio hooks; network type — **fixed (c4f1ea2): QUERYCAP capture nodes, checked sysfs writes, fprintd exit code, DT chassis-type form factor, scan-only BLE devices, `nmcli -e yes`; Android paths need the hook, Known Folders, user-dirs.dirs; audio samples both ways; haptic timing; kernel transport detection, link-local excluded** |
| E9 | B | Locale categories (`LC_TIME`/`LC_NUMERIC`), OS user overrides, console code page, `TERM`, mintty — **fixed (be71f3d, 314202b): `category` time/numeric, Windows/Apple user overrides; code page restored at exit, VT-mode check, mintty** |
| E10 | A | Android tzdata bundle; MinGW local zone; ISO-8859-9 — **fixed (6831dbf, 314202b): tzdata bundle index, registry TZI + windowsZones (MinGW), ISO-8859-9 → windows-1254** |
| E11 | D | CI: iOS builds only `forge_c` (apple.mm UIKit never compiled); Android runs no `test_mobile`/`test_locale` — **fixed (62d9687): iOS builds forge_mobile + forge_text; Android emulator runs test_mobile/test_locale/test_text** |

## Wave 2: new backends (after wave 1)
| ID | Item | Status |
|---|---|---|
| W2-1 | HTTPS in `net::http` (reuse FORGE's TLS when built; Schannel / Network.framework otherwise) | **done**: Schannel + crypt32 (Windows), Network.framework (Apple, CI only), dlopen'd OpenSSL ≥ 1.1 (Linux/FreeBSD/Android); verify on by default, `ca_file`/`ca_pem`, `insecure_skip_verify` (tests); tested Linux, FreeBSD 14, Windows (MinGW) against a local OpenSSL server + example.com. No revocation checks, client certs or connection reuse. (FORGE's rpc TLS links OpenSSL, so it is not reused.) |
| W2-2 | Windows services through the SCM API (env, working dir, status), per-user services through Task Scheduler; FreeBSD rc.d | done (f699fea): `service_scope` user/system; SCM API (description, restart failure actions, `Environment` REG_MULTI_SZ, working dir via `FORGE_SERVICE_DIR` + `run_as_service`, state/pid/exit code); per-user logon task through ITaskService COM (run-time loaded), env via encoded PowerShell wrapper, stop ends the process tree; FreeBSD rc.d + daemon(8) (system scope; user scope unsupported); Linux/macOS system scope. Tested: rendering/injection everywhere, real task on Windows, real rc.d on FreeBSD 14.5; SCM install CI/elevated only (render + query tested) |
| W2-3 | Windows process_pool sandbox through AppContainer; Linux Landlock for `deny_exec` and file policy | done: Windows net/fs workers run in an AppContainer (handle list, image ACE, token check in the worker); Landlock ABI 1-6 stacked on seccomp, `run_sandboxed` denies exec to the target (except `ld.so prog`); seccomp-only fallback, `FORGE_LANDLOCK=0` |
| W2-4 | Keystore key wrapping through TPM / Argon2id passphrase | **done**: Argon2id + XChaCha20-Poly1305 passphrase wrapping of `.master` (authenticated params header, minimum 8 MiB/2 passes, plain-to-wrapped migration, `rewrap`); opt-in TPM sealing (`FORGE_KEYSTORE_TPM=1`): tpm2-tss at run time on Linux/FreeBSD (tested with libtpms), NCrypt Platform Crypto Provider on Windows (tested on a real TPM); passphrase copy as recovery; `backend()` reports `file+argon2id`/`file+tpm` |
| W2-5 | Windows mobile backends (Sensor API, Media Foundation cameras, brightness, Bluetooth, audio, notifications) | done: ISensorManager sensors, MF enumeration + one-frame capture (WIC JPEG), WMI then DDC/CI brightness, classic Bluetooth inquiry, WASAPI shared-mode record/play, Shell_NotifyIconW notifications; all run-time loaded. Geolocation/BLE stay WinRT glue. Verified on a desktop without sensors, camera, radio or microphone (those paths unsupported) |
| W2-6 | Linux mobile backends (GeoClue, V4L2 capture, more IIO channels, logind brightness and inhibit); FreeBSD keep_awake/backlight | done: GeoClue2 location and logind SetBrightness / Inhibit over sd-bus, V4L2 MJPEG capture (DHT added), IIO channels by attribute name; FreeBSD backlight(9), keep_awake via xset under X11 only (no inhibitor exists). YUV-only cameras unsupported (no encoder) |
| W2-7 | Apple mobile backends (deviceMotion, pedometer, proximity, pasteboard, macOS notifications/cameras/location/biometrics) | fixed: iOS deviceMotion → rotation_vector/gravity/linear_acceleration on the shared CMMotionManager, CMPedometer step_counter/step_detector, UIDevice proximity (refcounted, main thread), UIPasteboard; macOS CoreLocation (async on the main run loop), AVCaptureDeviceDiscoverySession cameras + camera/location permission, LAContext Touch ID, NSPasteboard; macOS notifications (UNUserNotificationCenter / osascript) verified. Compiled in CI only (no Mac/device) |
| W2-8 | FSEvents on macOS; `load_average` emulation on Windows | FSEvents fixed: `file_watcher::add_tree` opens one recursive stream (CoreServices late-bound, FileEvents/NoDefer/WatchRoot, 50 ms, dispatch queue waking the kqueue through EVFILT_USER; drops → `overflow`); `tree_watcher` uses it (`fsevents = false` for kqueue); kqueue kept for `add`/`add_file`. macOS CI only | | FSEvents fixed: `file_watcher::add_tree` opens one recursive stream (CoreServices late-bound, FileEvents/NoDefer/WatchRoot, 50 ms, dispatch queue waking the kqueue through EVFILT_USER; drops → `overflow`); `tree_watcher` uses it (`fsevents = false` for kqueue); kqueue kept for `add`/`add_file`. macOS CI only Windows half done (20f3cac): 5 s sampler of busy CPUs + PDH processor queue length, exp(-5/60 / 300 / 900) decay, lazy start with a 100 ms first estimate |

## Inherent limits (not fixed)
Windows: `chown`, POSIX signals beyond kill/terminate, user-mode PMCs, THP, per-process open-file/file-size limits,
in-process network/file denial. macOS: hard CPU affinity, temperature without SMC. iOS: spawning, process listing,
services, light sensor. Android: JNI-only features (vibrate, notify, display, clipboard, services, keystore dir).
Linux: abstract sockets only. DPAPI and Keychain do not isolate secrets between processes of one user.
