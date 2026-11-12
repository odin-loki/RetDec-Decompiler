# memory: budgets, eviction and backpressure

## Concepts
FORGE's memory manager keeps one view of how much memory the process may use, who uses it, and what can be dropped, so work slows down or fails cleanly instead of being OOM-killed. When memory runs short FORGE reacts in this order:

1. Drop caches.
2. Hold back new work.
3. Wait for running work to free memory.
4. Fail the one request that cannot fit, with a clear error.

Code: `core/include/forge/core/memory.hpp` (namespace `forge::mem`, header-only). Device pools: `gpu/include/forge/gpu/device_memory.hpp`. Tests: `tests/test_memory.cpp`, `tests/serve_test.py`, the `test_gpu_memory_*_mock` tests and `test_vulkan`.

- **Budget.** A named pool with a byte limit. `host` is the process heap and anonymous memory. Device pools (`cuda:0`, `vulkan:0`, ...) are created on demand with `governor::instance().pool(name)`.
- **Hard limit and soft watermark.** The soft watermark is `FORGE_MEM_SOFT × limit` (0.9 by default). With no limit every check is a single compare and the pool only keeps accounts.
- **Errors.** A request that cannot fit raises `errc::out_of_memory` (a `forge_exception`). `forge-serve` turns it into HTTP 503. The message names the pool, the tag, the size and the setting to change.
- **Weights are not charged.** `.fpk` tensors are memory-mapped (`core/mapped.hpp`); the kernel pages them in on demand and can drop clean pages. `FORGE_HUGEPAGES=1` asks for transparent huge pages on large mappings.

## API reference
| API | Meaning |
|---|---|
| `governor::instance()`, `host_budget()` | Process-wide registry; `host()`, `pool(name)`, `pools()`, `describe()` (text for `forge info`), `prometheus()` |
| `budget::try_reserve(n, tag)` | Non-blocking. If `n` does not fit, asks caches to free the shortfall; returns an empty `lease` if it still does not fit (counted in `denials`) |
| `budget::reserve(n, tag, timeout)` | As `try_reserve`, then waits up to `timeout` for leases to be released, evicting again each time it wakes. Throws `out_of_memory` on timeout, with no timeout, or when `n` exceeds the whole limit |
| `lease` | RAII hold on bytes: `bytes()`, `resize(n)`, `release()`; movable |
| `budget::set_usage(tag, n)` | Report a self-sizing consumer (live KV cache). Counted, never blocked |
| `budget::add_cache(name, size_fn, evict_fn, priority)` | Register something droppable. Eviction goes lowest priority first, then least recently touched (`touch_cache`). `remove_cache` |
| `budget::evict(bytes)`, `trim()` | Free about `bytes` from caches (0 = everything); `trim` evicts down to the soft watermark |
| `budget::under_pressure(extra)` | `used + cached + extra` is over the soft watermark |
| `budget::set_limit`, `set_soft_fraction`, `stats()` | Limit, watermark, counters (`reservations`, `denials`, `waits`, `evictions`, `evicted_bytes`, `peak`, `by_tag`) |
| `governor::watch_pressure()` | Subscribe to `os::memory_pressure`: moderate trims every pool to its soft watermark, critical drops every cache. Off with `FORGE_MEM_PRESSURE=0` |
| `budget_resource(budget, tag, upstream)` | `std::pmr::memory_resource` that charges a budget and throws `out_of_memory` past the limit. Alignment goes straight to the upstream (default `new_delete_resource`) |
| `spill_store(dir)` | Numbered files for evicted blobs: `put` (returns 0 when over `FORGE_MEM_SPILL_MAX`), `take` (reads and deletes), `drop`, `bytes()`. Removes its files on destruction |
| `spawn_leased(budget, spec, bytes, tag, wait)` | Reserve `bytes`, start an `os::child` with that as its memory cap, return a `leased_child` that releases the lease when the child is reaped |
| `parse_size`, `format_size`, `detect_host_limits()` | `"6G"`, `"512M"`, `"1.5GiB"`, `"75%"`; physical, available, cgroup and `RLIMIT_AS` figures |

Cache `size_fn` callbacks run while the budget's lock is held, so they must not call back into the budget. `evict_fn` callbacks run without the lock and may.

## How the host limit is chosen
Highest priority first:
1. `forge-serve --mem-limit` or `budget::set_limit` in code.
2. `FORGE_MEM_LIMIT`: a size (`6G`, `512M`, `1.5GiB`, bytes) or a percentage of the detected limit (`75%`). `0`, `off` or `none` turns the limit off.
3. `FORGE_MEM_FRACTION` (default 0.85) of the detected limit.

The detected limit is the smallest of physical RAM, the tightest cgroup limit found walking up from `/proc/self/cgroup` (`memory.max` for v2, `memory.limit_in_bytes` for v1), and `RLIMIT_AS`. Device pools take their limit from `FORGE_MEM_DEVICE_LIMIT`.

## What uses it
| Consumer | Behaviour |
|---|---|
| Batch scheduler: running KV | Reports live KV bytes each step with `set_usage("kv", ...)` |
| Batch scheduler: admission control | Before admitting a queued request it projects the cost from the largest KV size seen per request. If that crosses the soft watermark it evicts prefix-cache entries one at a time (least recently used first), then leaves the request queued. At least one request always runs, so the queue drains. Output is identical, only less parallel. Counted in `deferred` |
| Batch scheduler: prefix KV cache | Registered as a cache (priority 10). An eviction request from the budget or the pressure response drops the whole prefix cache at the next scheduler step |
| Prefix KV spill | With `batch_options::spill` set (`forge-serve --kv-spill`), an evicted prefix is saved to the `spill_store` (`session::save_prefix`, up to `spill_entries`, default 64) instead of dropped. A prompt that shares a spilled prefix reads it back (`session::load_prefix`) and uses it like an in-memory hit, so logits are bit-identical to recomputing. If the store is full the prefix is dropped |
| `forge-serve` backpressure | `--max-queue N` rejects new requests with 503 and `Retry-After: 1` once N are waiting. `out_of_memory` or `std::bad_alloc` while handling a request returns 503 |
| Paged KV | `session_options.page_rows` (default 256) grows the KV cache a page at a time |
| Text library, mobile queues | Collators and encoder tables lease from `host` (tag `text`); mobile event queues allocate through `budget_resource` (tag `mobile`) |

## GPU memory
CUDA, HIP, OpenCL, Level Zero and Vulkan charge the pool `<backend>:<ordinal>` (`cuda:0`, `hip:0`, `opencl:0`, `level_zero:0`, `vulkan:0`) for every driver allocation, with one lease per allocation released on free. Tags: `resident` (tensors kept on the device by the residency protocol, and freed buffers that the CUDA/HIP runtime pools for reuse), `weights` (encoded weights for the quantised kernels), `scratch` (per-op temporaries and staging) and `raw` (`device_alloc`).

Each runtime registers its buffers with the pool as a cache. Before an allocation would cross the limit, and when the driver itself reports out of memory, the runtime makes room:
1. It frees its pooled free buffers (CUDA/HIP).
2. It evicts resident tensors and encoded weights, least recently used first. A tensor whose newest data is only on the device is copied back to the host first; the next op that needs it uploads it again. This is `FORGE_GPU_NO_RESIDENCY` behaviour applied on demand to just enough buffers.

Buffers bound by the op in progress are never evicted. If nothing more can be evicted the allocation fails with `out_of_memory`, and the runtime stays usable for later ops that fit. Eviction requested from another thread (pressure response, another runtime on the same pool) only `try_lock`s the runtime and skips a busy one, so runtimes cannot deadlock.

- Eviction moves device addresses, so the CUDA runtime turns off whole-run replay until runs are confirmed again.
- Nothing is evicted while a CUDA/HIP stream capture or an OpenCL/Level Zero graph recording is open, and a graph recorded before an eviction must not be replayed after it.
- `residency()` counts `evictions` and `evicted_bytes`; `memory_pool()` and `charged_bytes()` return the pool and the bytes the runtime holds.
- **Metal** has unified memory and no residency. Each op charges its buffers to `metal:<ordinal>` (tag `scratch`) only while it runs; nothing is evicted, and an op that does not fit fails.

Tested with mock drivers (`test_gpu_memory_{cuda,hip,opencl,ze}_mock`) and on Mesa lavapipe for Vulkan (`test_vulkan`). Not run on real GPUs.

## Observability
- `forge info` prints physical RAM, available RAM, any cgroup limit and usage, any `RLIMIT_AS`, the `host` budget and soft watermark, and where the limit came from.
- `forge-serve` prints the memory budget (or `off`) at startup.
- `GET /metrics` adds, per pool, `forge_mem_limit_bytes`, `forge_mem_used_bytes`, `forge_mem_peak_bytes`, `forge_mem_cache_bytes`, the counters `forge_mem_reservations_total`, `forge_mem_denials_total`, `forge_mem_waits_total`, `forge_mem_evictions_total`, `forge_mem_evicted_bytes_total`, and `forge_mem_used_bytes_by_tag{pool,tag}`. It also adds `forge_mem_pressure_events_total` and the scheduler metrics `forge_queue_waiting`, `forge_queue_active`, `forge_admission_deferred_total`, `forge_rejected_total`, `forge_kv_evictions_total`, `forge_kv_spills_total`, `forge_kv_reloads_total` and `forge_kv_spilled` (prefixes on disk).

## Configuration
| Setting | Default | Effect |
|---|---|---|
| `FORGE_MEM_LIMIT` | unset | Host limit: a size, a percentage of the detected limit, or `off` |
| `FORGE_MEM_FRACTION` | `0.85` | Share of the detected limit used when `FORGE_MEM_LIMIT` is unset |
| `FORGE_MEM_SOFT` | `0.9` | Soft watermark as a share of the limit |
| `FORGE_MEM_DEVICE_LIMIT` | unset | Limit for every device pool |
| `FORGE_MEM_PRESSURE` | on | `0` ignores OS pressure signals |
| `FORGE_MEM_SPILL_DIR` | `<tmp>/forge-spill-<pid>` | Spill directory |
| `FORGE_MEM_SPILL_MAX` | unlimited | Maximum bytes in the spill store |
| `forge-serve --mem-limit` | env | Same values as `FORGE_MEM_LIMIT`; takes priority |
| `forge-serve --max-queue N` | `0` (no cap) | Reject with 503 once N requests wait |
| `forge-serve --kv-spill` | off | Spill evicted prefix KV to disk and reload it on a hit |

In a container the cgroup limit is detected, so the defaults are usually enough. To set the limit and queue cap explicitly:

    forge-serve model.fpk --mem-limit 3G --max-queue 32 --max-active 4

## Platform notes
| Platform | Limit | Pressure source |
|---|---|---|
| Linux, Android | cgroup v1/v2, physical RAM, `RLIMIT_AS`; available memory from `MemAvailable` | PSI triggers (Android also `onTrimMemory` through the app) |
| macOS, iOS | physical RAM or `RLIMIT_AS`. `os::available_memory()` is a rough estimate (a quarter of RAM), so set `FORGE_MEM_LIMIT` | dispatch memory-pressure source |
| Windows | physical RAM. A Job Object limit is not detected; set `FORGE_MEM_LIMIT` | `CreateMemoryResourceNotification` |
| FreeBSD | physical RAM or `RLIMIT_AS` | 1 s poll of free memory |

## Not built yet
- Per-request KV reservation sized from `(prompt + max_tokens) × KV bytes per token`, so a request that can never fit is rejected at submission.
- Budget awareness in `forge run` and the C ABI entry points.
- `os_proc_available_memory` on iOS and Job Object limit detection on Windows.

CPU attention reads int8 and f32 KV slots in place through the chunked kernel (`graph/attn_cpu.hpp`), and int8 rows are encoded on the task pool. fp8 and tbq slots still use the per-(token, head) decode path.
