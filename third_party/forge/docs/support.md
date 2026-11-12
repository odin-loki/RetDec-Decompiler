# support: logger, pools, cache, settings and helper libraries

Utilities used across FORGE. All are header-only.

| Header | Namespace | What | Tests |
|---|---|---|---|
| `core/include/forge/core/log.hpp` | `forge::log` | Logger | `test_log`, `test_log_strip`, `log_min_level_cmake` |
| `support/include/forge/task/pool.hpp` | `forge::task` | Thread pool | `test_pool` |
| `os/include/forge/os/process_pool.hpp` | `forge::os` | Process pool (see [os-wrapper.md](os-wrapper.md#process-pools-process_poolhpp)) | `test_process_pool` |
| `os/include/forge/os/process.hpp`, `core/include/forge/core/memory.hpp` | `forge::os`, `forge::mem` | Waiting on processes, leased children | `test_os_process`, `test_memory` |
| `support/include/forge/cache/cache.hpp` | `forge::cache` | Content-addressed disk cache | `test_cache` |
| `os/include/forge/os/time.hpp`, `os/include/forge/os/fs.hpp` | `forge::os` | `stopwatch`, `deadline`, atomic writes | `test_util` |
| `os/include/forge/os/settings.hpp` | `forge::os::settings` | Environment-variable registry | `test_util` |
| `support/include/forge/...` (below) | various | Hashing, I/O, JSON, maths, allocators, profiling, RNG, reflection, signal, vision | various |

## Logger (`forge::log`)
```cpp
forge::log::info("loaded {} tensors", n);                 // module "forge", std::format syntax
forge::log::logger L{"inference"}; L.debug("kv {}", b);   // module "inference"
FORGE_LOG_DEBUG("graph", "fused {} ops", k);              // arguments not evaluated when disabled
```

| Feature | API |
|---|---|
| Levels | `trace`, `debug`, `info`, `warn`, `error`, `fatal`, `off`; default `warn` |
| Thresholds | `FORGE_LOG="info,inference=debug,graph=trace"` (global, then per module; the first letter of a level is enough). At run time: `set_level`, `set_module_level`, `configure(spec)` (resets module overrides), `threshold(mod)`, `enabled(level, mod)` |
| Format | Text `[forge W] msg` / `[forge:module W] msg`, or JSON lines (`ts`, `level`, `module`, `thread`, `msg`) with `FORGE_LOG_FORMAT=json` or `set_format` |
| Built-in stderr | On by default; `set_stderr(false)` turns it off |
| Sinks | `add_sink(fn)` / `remove_sink(id)`; `add_file_sink(path, max_bytes, keep)` (size rotation `path.1 … path.<keep>`); `add_syslog_sink(ident)` (POSIX; journald picks it up); `add_eventlog_sink(source)` (Windows). Each has a `make_*_sink` form (`make_stderr_sink`, `make_file_sink`, `make_syslog_sink`, `make_eventlog_sink`) that returns the sink without registering it, empty on failure. `add_*` return 0 on failure |
| Async | `add_async_sink(sink, capacity = 8192)`: a bounded queue drained by one thread named `forge-log` |
| Compile-time strip | `FORGE_LOG_MIN` or `FORGE_LOG_MIN_LEVEL` = 0..6 (trace..off): calls below compile to nothing |

- **Synchronous sinks** run inside the log call: each line is formatted first and written under one lock, so lines never interleave.
- **Async sinks.** A log call only copies the record into the queue. When the queue is full the line is dropped, never waited for. Drops are counted (`async_sink_stats(id)`: written, dropped, pending) and later reported to the sink as a warn record of module `log`: `dropped N log line(s): async queue full (capacity C)`. `flush()` waits until everything queued before it is written. `remove_sink(id)` and process exit drain the queue and join the thread. The wrapped sink runs on that thread, one line at a time, outside the logger lock. A throwing sink does not stop the thread. For fully non-blocking logging: `set_stderr(false); add_async_sink(make_stderr_sink());`.
- **A sink must not log** (the sink lock is held while it runs; an async sink would feed its own queue).
- **Windows event log.** `RegisterEventSourceW` + `ReportEventW` to the Application log. error/fatal are error events, warn is a warning, the rest are information. Event id 0 and no message file, so Event Viewer shows the line after a "description for Event ID 0 cannot be found" note. `forge_core` links `advapi32` on Windows.
- **Compile-time strip.** The `FORGE_LOG_TRACE(mod, ...)` … `FORGE_LOG_FATAL` macros also skip evaluating their arguments when the level is disabled at run time; the functions always evaluate them. The CMake cache variable `FORGE_LOG_MIN_LEVEL` (`trace` … `off`, default `none`) defines the macro for every target linking `forge_core` ([building.md](building.md)). With it set, `test_log` reports skipped.

## Thread pool (`forge::task`)
| API | Meaning |
|---|---|
| `pool(n)` | `n = 0` uses `FORGE_THREADS`, then `hardware_concurrency()` |
| `pool::global()`, `task::parallel_for`, `task::submit` | One shared pool for every layer, so nothing oversubscribes |
| `parallel_for(n, grain, f(begin, end))` | Data-parallel loop; the caller participates. These jobs run before queued tasks |
| `submit(f, {priority, stop_token})` | Returns a `std::future`. Higher priority first, FIFO within a priority. A task whose token is triggered before it starts is skipped and its future throws `task::cancelled`; a running task sees the token if `f` takes a `std::stop_token` |
| `pending()`, `wait_idle()`, `size()` | Queue length, wait for an empty queue, worker count |

The destructor still runs (or skips, if cancelled) queued tasks before the workers exit.

## Processes
- `os::spawn_spec` and `spawn_wait` (argv, env, cwd, limits, output cap): see [os-wrapper.md](os-wrapper.md#processes-processhpp).
- `os::wait_any(children, timeout_ms)` returns the index of the first `child` to exit (reaped), or `nullopt` on timeout. `os::wait_all(children, timeout_ms)` returns false on timeout. Both poll with a 1–10 ms back-off and never reap another waiter's child.
- `mem::spawn_leased(budget, spec, bytes, tag, wait)` reserves `bytes` from a `forge::mem` budget, uses it as the child's memory cap, and releases it when the child is reaped ([memory.md](memory.md)). POSIX only, since it uses `os::child`.

## Cache (`forge::cache`)
```cpp
cache::store c(dir, {.max_bytes = 1 << 30});
auto k = c.put("blob");                          // key = BLAKE3 of the content
auto m = cache::key_of({"solver", query, opts}); // memo key from several inputs (length-prefixed)
c.put(m, result); if (auto v = c.get(m)) use(*v);
```
- Layout `<dir>/<2 hex>/<62 hex>`. Each file is the value's 32-byte BLAKE3 followed by the value.
- `get()` verifies the hash and deletes a corrupt entry. It refreshes the entry's mtime.
- Writes go through `os::fs::write_file_atomic`, so readers, including other processes, see a whole entry or none.
- With `max_bytes` set, `put()` evicts least recently used entries until the store fits. `erase`, `clear`, `prune`, `stats()` (entries, bytes, hits, misses, evictions, corrupt). Thread-safe.

## Time and files
- `os::stopwatch`: `elapsed_ns/ms/s`, `lap_ns`, `restart`.
- `os::deadline`: an absolute monotonic time. `after_ms`, `after_ns`, `never()`, `expired`, `remaining_ns/ms`, `sleep`. `child(ms)` makes a sub-deadline that never extends past the parent.
- `os::fs::write_file_atomic(path, data, mode)`: temp file, fsync, rename, then fsync of the directory on POSIX.

## Settings registry (`forge::os::settings`)
One documented table for every environment variable a program reads.

```cpp
inline const auto& kTimeout = settings::define<std::int64_t>("MYAPP_TIMEOUT_MS", 30000, "Per-query timeout");
auto t = kTimeout.get();          // parsed and defaulted; re-parsed when the variable changes
```
| API | Meaning |
|---|---|
| `define<T>(name, default, help)` | Kinds: string, int (decimal or `0x` hex; `010` is ten; out of range for `T` is a problem), bool (`1/0/true/false/yes/no/on/off`), size (K/M/G/T suffixes, base 1024), real. Windows reads the UTF-16 environment |
| `get_raw(name)` | Raw value; throws for a name that was never defined, so stray `getenv` calls can be replaced one by one |
| `reload()`, `problems()` | Re-read the environment; values that did not parse (the default is used) |
| `markdown(prefix)`, `all()`, `defined(name)` | The table as Markdown, all entries |
| `undocumented(prefix)` | Variables set in the environment with the prefix but never defined (typos) |

## Helper libraries (`support/include/forge/`)
| Header | Namespace | Contents |
|---|---|---|
| `hash/blake3.hpp` | `forge::hash` | Portable BLAKE3 (32-byte hash mode): `update`, `final`, `of`, `hex`. Used for `.fpk` integrity and the cache |
| `io/aio.hpp` | `forge::io` | `async_reader`: io_uring through raw syscalls on Linux, else a `pread` thread pool. Short reads are resubmitted, so a completion covers the whole request |
| `json/json.hpp` | `forge::json` | Small strict JSON reader/writer (`value`, typed accessors, `num_or`, ...) |
| `linalg/linalg.hpp` | `forge::linalg` | Row-major `mat<T>`: `matmul`, LU, Cholesky, Householder QR, CSR `spmv`, conjugate gradients, GMRES, Jacobi `sym_eig` and `svd` |
| `mem/arena.hpp` | `forge::mem` | Bump `arena` with `save`/`restore` marks and RAII `scope`; any power-of-two alignment |
| `mem/tlsf.hpp` | `forge::mem` | Fixed-block `pool` and O(1) `tlsf` allocator over a caller region, with stats and `leak_report` |
| `prof/prof.hpp` | `forge::prof` | `FORGE_ZONE(name)` scoped zones per thread, exported as Chrome trace JSON; a disabled zone costs one relaxed load |
| `rand/philox.hpp` | `forge::rand` | Counter-based `philox4x32` (10 rounds) and `threefry4x32` (20 rounds) |
| `reflect/reflect.hpp` | `forge::reflect` | `FORGE_REFLECT(T, fields...)` (up to 12 fields), `to_json`, `from_json` |
| `signal/fft.hpp` | `forge::signal` | FFT (radix-2, Bluestein for other lengths), `rfft`, Hann window, STFT power, Slaney mel filterbank |
| `signal/audio.hpp` | `forge::signal` | Windowed-sinc `resample`, RBJ `biquad` filters |
| `vision/vision.hpp` | `forge::vision` | Bilinear resize, RGB↔YUV (BT.601 full range), IoU, greedy NMS, `roi_align` |
| `vision/video.hpp` | `forge::vision::video` | Hardware decode interface (NVDEC, MediaCodec, VideoToolbox; VA-API probe only), Annex-B and H.264 SPS parsing. Only the parsing is tested; the decoders have not run on hardware |
