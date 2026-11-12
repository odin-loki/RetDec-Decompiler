# loadbalancer: load balancer and proxy

Headers: `loadbalancer/include/forge/loadbalancer/loadbalancer.hpp` (pool) and `proxy.hpp` (L4/L7 proxy). Header-only; target `forge_loadbalancer`. Namespace `forge::loadbalancer`.

## Concepts
A `pool` holds backends and picks one per request with a **strategy**. A pick returns a `lease`; you report the outcome on it (success, failure, latency). The pool uses those reports for:
- in-flight counts and EWMA latency,
- a per-backend circuit breaker (closed → open after consecutive failures or a high error rate → half-open trial requests → closed).

On top of that:
- active health checks (`set_health_check` + `health_checker`, with hysteresis),
- `pool::call`: retries on a *different* backend with exponential backoff and jitter,
- per-backend `max_inflight` limits with a bounded, deadline-aware wait queue (backpressure),
- dynamic membership: `add`, `remove` (drains), `set_weight`, `set_healthy`.

The pool is thread-safe. Time comes from `forge::os::monotonic_ns`. `proxy.hpp` builds the TCP (L4) and HTTP/1.1 (L7) proxies used by `forge-serve proxy`.

## API reference

### Strategies

| `strategy` | Names for `parse_strategy` | Behaviour |
|---|---|---|
| `round_robin` | `rr`, `round_robin` | Each usable backend in turn |
| `weighted` | `wrr`, `weighted` | Smooth weighted round robin |
| `least_connections` | `lc`, `least_connections` | Fewest in flight (ties: fewest requests) |
| `least_latency` | `ewma`, `least_latency` | Lowest (EWMA latency + 1) × load. Old estimates decay toward the pool mean; unmeasured backends get explored |
| `consistent_hash` | `hash`, `ring`, `consistent_hash` | Hash ring on `pick_options::key`, `vnodes` points per unit weight, bounded load |
| `power_of_two` | `p2c` (default), `power_of_two` | Two random candidates, picks the lower (latency + 1) × load |
| `prefix_affinity` | `kv`, `prefix`, `prefix_affinity` | Hashes `pick_options::prefix` in blocks and sends it to the backend that last took the longest matching prefix (it holds that KV cache), unless that backend's load exceeds `avg × prefix_spill + 1` |

`parse_strategy(name)` returns `std::optional<strategy>`; `strategy_name(s)` gives the long name. A half-open backend waiting for a trial request is always picked first.

### Types
- `backend_spec`: `id` (unique), `address` (free-form; the proxy needs `host:port`), `weight` = 1.0, `max_inflight` = 0 (unlimited), `user` (opaque pointer).
- `config` (defaults):

  | Group | Fields |
  |---|---|
  | Strategy | `strat = power_of_two` |
  | Consistent hash | `vnodes = 160`, `hash_load_factor = 1.25` |
  | EWMA | `ewma_alpha = 0.3`, `ewma_decay_ns = 10 s` |
  | Circuit breaker | `failure_threshold = 5` consecutive, or `failure_rate_threshold = 0.5` over the last `window = 20` with at least `min_requests = 10`; `open_ns = 5 s`; `half_open_probes = 1` |
  | Health checks | `healthy_after = 2`, `unhealthy_after = 3` |
  | Prefix affinity | `prefix_block = 64` bytes, `prefix_table = 65536` blocks (cleared when full), `prefix_spill = 1.5` |
  | Backpressure | `queue_limit = 1024` waiting acquirers |

- `pick_options`: `key` (hashing), `prefix` (prefix affinity), `exclude` (indices already tried), `wait_ns` (0 = fail fast).
- `retry_policy`: `attempts = 3`, `base_backoff_ns = 2 ms`, `max_backoff_ns = 200 ms`, `wait_ns = 0`.

### `pool`

| Method | Meaning |
|---|---|
| `pool(config)` | |
| `add(spec)` → index | Re-adding a removed id reuses its slot |
| `remove(id)`, `set_weight(id, w)`, `set_healthy(id, h)`, `set_strategy(s)` | `remove` drains: in-flight leases finish normally |
| `acquire(pick_options = {})` → `lease` | Empty lease (`!l`) when nothing is usable, or after `wait_ns` / a full queue |
| `call(pick_options, fn(const backend_spec&) → bool, retry_policy = {})` | Runs `fn` on up to `attempts` different backends; an exception counts as failure. Returns whether one succeeded |
| `set_health_check(fn)`, `run_health_checks()` | Probe all backends once |
| `stats()` | Per backend: `id`, `healthy`, `state`, `inflight`, `requests`, `failures`, `ewma_ms`, `weight` |
| `size()`, `waiting()`, `rejected()`, `spec(i)` | |

`lease`: `backend()`, `index()`, `success()` (latency measured from acquire), `success_with_latency(ns)`, `failure()`. A lease destroyed without a report counts as a failure.

`health_checker(pool&, period_ns)` runs `run_health_checks()` on its own thread until destroyed. `tcp_probe(timeout_ms = 500)` (in `proxy.hpp`) is a probe that tries to connect to `address`.

### Proxy (`proxy.hpp`)
`proxy(shared_ptr<pool>, proxy_config)`, `start()` → `os::status`, `stop()`, `port()`, `served()`, `errors()`.

| `proxy_config` field | Default | Meaning |
|---|---|---|
| `mode` | `l4` | `l4` or `l7` |
| `listen_host`, `listen_port` | `127.0.0.1`, 0 | 0 = any free port |
| `connect_timeout_ms`, `idle_timeout_ms` | 2000, 60000 | |
| `retries` | 3 | Backends tried per request / connection |
| `queue_wait_ns` | 1 s | Backpressure wait |
| `affinity_header` | `x-session-id` | Lower-case |
| `max_header_bytes` | 64 KiB | |
| `routes` | none | `{path_prefix, pool}`; longest matching prefix wins, otherwise the default pool |

- **L4**: each client connection leases one backend for its lifetime and relays bytes both ways. A backend that refuses the connection is retried on another.
- **L7**: parses each request (Content-Length or chunked body), picks a pool by path prefix, and acquires with `key` = the affinity header or, if absent, the request target, and `prefix` = the request body. Unreachable upstreams are retried on another backend; `502` / `503` answers are retried only for idempotent methods (GET, HEAD, PUT, DELETE, OPTIONS) or requests with an `Idempotency-Key` header. Client connections keep alive; each upstream request uses its own connection with `Connection: close` and `X-Forwarded-By: forge`. When no backend answers, the client gets `503`.

### `forge-serve proxy`

```
forge-serve proxy --backend HOST:PORT [--backend ...] [--route /prefix=host:port,host:port]...
    [--mode l7|l4] [--strategy p2c] [--host 127.0.0.1] [--port 8080] [--retries 3] [--health-ms 2000]
```

| Flag | Default | Effect |
|---|---|---|
| `--backend` | — | Adds a backend (`id` = `address` = the value, weight 1). At least one `--backend` or `--route` is needed |
| `--route` | — | Path prefix served by its own pool of the listed backends |
| `--mode` | `l7` | |
| `--strategy` | `p2c` | Any `parse_strategy` name; also used by route pools |
| `--host`, `--port` | `127.0.0.1`, 8080 | |
| `--retries` | 3 | |
| `--health-ms` | 2000 | TCP health-check period for the default pool. Route pools are checked every 2 s |

It runs until SIGINT/SIGTERM and then prints the served and error counts.

## Example
`docs/examples/loadbalancer_pool.cpp` (ctest `doc_example_loadbalancer_pool`):

```cpp
config c; c.strat = *parse_strategy("p2c");
pool p(c);
p.add({"gpu0", "10.0.0.5:7000", 1.0});
p.add({"gpu1", "10.0.0.6:7000", 1.0});
{ lease l = p.acquire(); l.success_with_latency(1500); }   // ns; feeds the EWMA
bool ok = p.call({}, [&](const backend_spec& b) { return send_to(b); }, {3, 1000, 10000});
```

Proxy in front of two servers:

```sh
forge-serve model.fpk --port 9001 & forge-serve model.fpk --port 9002 &
forge-serve proxy --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 --strategy kv --port 8080
```

With `kv`, requests whose bodies share a prompt prefix go to the same server, where `forge-serve`'s prefix cache can reuse the KV rows.

## Platform notes and limits
- `tests/test_loadbalancer.cpp` covers every strategy, the circuit breaker, health checks, retries, backpressure, dynamic membership and both proxy modes.
- The proxy speaks HTTP/1.1 only, without TLS, and buffers each upstream response fully before relaying it (no streaming pass-through, so SSE responses arrive at the end).
- L4 affinity uses the socket handle as the key, so it is not stable per client.
- `prefix_affinity` hashes raw bytes, so prompts only match block-for-block from the start of the body.
