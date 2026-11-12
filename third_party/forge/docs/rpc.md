# rpc: remote devices, authentication, TLS and remote scripts

`forge rpc-serve` lends a machine's devices, and optionally a script runtime, to other FORGE processes. Code: `graph/include/forge/graph/rpc.hpp` (protocol, client, server) and `rpc_secure.hpp` (TLS and authentication). Tests: `tests/test_rpc.cpp` (ctests `test_rpc` and `test_rpc_tls`).

## Concepts
- **Remote device.** A client uses the device `rpc:host:port` (for example `FORGE_DEVICE=rpc:box2:7000`), or a pipeline stage such as `pipe:rpc:box2:7000,cpu` (see [graph.md](graph.md)). Each op is sent as an `FRPC` frame with its inputs; the server runs it on its own device registry and returns the output. Every op is still checked by the oracle on the client. If the connection fails, the client falls back to the CPU.
- **Remote script.** An `FRPS` frame carries ForgeScript source; the server runs it in a sandboxed child process and returns `{ok, error_type, error, output}`. Clients are `forge::remote` and `forge::remote_call` ([scripting.md](scripting.md)).
- **Security.** Optional TLS (OpenSSL), then optional authentication with tokens or Ed25519 keys, checked before the first frame. Tokens, keys and client certificates can each be limited to device ops or scripts (`allow=`); a request must be allowed by every layer that applies.

## `forge rpc-serve` reference

```
forge rpc-serve [--port 7000] [--bind 127.0.0.1] [--device cpu|vulkan|cuda:0|...]
    [--token T | --token-file F]... [--authorized-keys FILE | --pubkey HEX|FILE]...
    [--tls-cert C --tls-key K [--tls-client-ca CA [--tls-client-map FILE]...]] [--quiet]
    [--allow-scripts [--script-timeout MS] [--script-mem-mb N] [--script-no-proc] [--script-no-fs] [--script-no-net]]
    [--policy FILE]
```

| Flag | Default | Effect |
|---|---|---|
| `--port`, `--bind` | 7000, `127.0.0.1` | Listen address |
| `--device` | `cpu` | Device that runs the ops. An unavailable device falls back to `cpu` with a warning |
| `--token T` | — | Accepted bearer token with full access (repeatable). `--token 'allow=device T'` takes a token-file line instead. Prefer `--token-file`: command lines are visible to other local users |
| `--token-file F` | — | Tokens, one per line, with optional permissions (format below; repeatable) |
| `--authorized-keys FILE` | — | Ed25519 public keys with optional permissions (format below; repeatable) |
| `--pubkey HEX\|FILE` | — | One public key (64 hex, or a `.pub` file from `forge keygen`) with full access (repeatable) |
| `--tls-cert`, `--tls-key` | — | Serve TLS only (TLS ≥ 1.2). Must be given together |
| `--tls-client-ca CA` | — | mTLS: require a client certificate that chains to `CA`. Needs `--tls-cert`/`--tls-key` |
| `--tls-client-map FILE` | — | mTLS: accept only the client certificates listed, each with permissions (format below; repeatable). Needs `--tls-client-ca` |
| `--quiet` | off | No security log (otherwise each connection's principal, permissions, refusals and denials go to stderr) |
| `--allow-scripts` | off | Accept `FRPS` frames. Requires tokens or keys (a client certificate alone never runs scripts); the server refuses to start without them |
| `--script-timeout MS` | 60000 | Per-script limit; a client may ask for less, not more |
| `--script-mem-mb N` | 2048 | Memory limit of the script child |
| `--script-no-proc`, `--script-no-fs`, `--script-no-net` | off | Deny those capabilities in the child (see Remote scripts) |
| `--policy FILE` | `load_policy()` search | FORGE Security Standard policy (see Security policy) |

With no tokens, keys or client map the server accepts anyone (legacy mode). It then warns if bound to anything other than `127.0.0.1`, `::1` or `localhost`, unless it requires client certificates.

### Security policy
With a policy (`--policy FILE`, `$FORGE_SECURITY_POLICY`, `./forge-security.json`, the config dir or `$FORGE_SECURITY_PROFILE`; see [security-api.md](security-api.md)), before serving:

1. `--bind`/`--port` must pass `enforce(net_listen)`: loopback always, any address only with `net.mode` `any` (or a `net.hosts` match in mode `hosts`). Otherwise the server does not start (`permission_denied`, exit 1).
2. `--allow-scripts` needs `exec` (each script runs in a `forge script` child). A policy that denies it (`strict`) stops the server at startup with "denies exec".
3. After the socket is bound and TLS material is loaded, the server calls `security::lockdown(policy)` (SBX-1): limits, no-new-privs, Landlock write roots, seccomp (`net.mode` `none` denies new sockets but the bound listener keeps accepting), job object / mitigations on Windows. Controls that are not `ok` are printed to stderr; the lockdown and every weakened control are audited.
4. Script children are started with `--policy <absolute path>` when one was given (otherwise they find the policy the way the server did), and lock themselves down before running the script ([scripting.md](scripting.md), "Security policy").

No policy (profile `none`): none of this happens. Test: `tests/security_cli_test.py` (strict rpc-serve on loopback, refusals, a remote script confined to the policy's roots).

## Authentication
Client and server exchange an `FRPH` hello naming a method:

| Method | Transport | Exchange |
|---|---|---|
| 1 token | plain TCP | Client sends the token; compared in constant time |
| 2 key | plain TCP | Client sends its public key, server sends a 32-byte CSPRNG nonce, client signs `"forge-rpc-auth-v1" ‖ nonce ‖ pubkey` |
| 3 bound token | TLS | Client sends `BLAKE2b-512(key = BLAKE2b-512(token), "forge-rpc-token-v2" ‖ cb)`; the token itself never crosses the wire |
| 4 bound key | TLS | As 2, signing `"forge-rpc-auth-v2" ‖ nonce ‖ pubkey ‖ cb` |

Keys use FORGE's `.fpk` signing code (vendored Monocypher) and the same files as `forge keygen` (secret = 64-hex seed, public = 64 hex). A client with both a key and a token uses the key.

### authorized_keys and per-key permissions
One key per line; blank lines and lines starting with `#` are ignored. As in ssh, options may come before the key; the rest of the line is a comment:

```
# [options] <64-hex ed25519 public key> [comment]
d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a alice        # no options: full access
allow=device 3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c gpu-borrower
allow=scripts fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025 ci
```

| Option | Meaning |
|---|---|
| `allow=PERM[,PERM...]` | The key may only do these. Repeated `allow=` options add up. `allow=` with no value authenticates but permits nothing |

| Permission | Allows |
|---|---|
| `device` | `FRPC` frames: ops on the server's device (`rpc:` devices, `pipe:` stages) |
| `scripts` | `FRPS` frames: remote scripts; the server must also run with `--allow-scripts` |
| `all` | Both. The default for a line without options, so older files keep working |

Unknown options or permissions, or a line without a 64-hex key, make `rpc-serve` refuse to start (fail closed), naming the file and line. A refused request gets `permission denied: ...` and the connection stays usable; `forge::remote` raises it as `PermissionError`.

### Token files and per-token permissions
Same format and permissions as authorized_keys, with a token in place of the key; the rest of the line is a name used in the server log (the token itself is never logged):

```
# [allow=PERM[,PERM...]] <token> [name]
allow=device 7f3c9e1d5a2b8c4e6f0a1b2c3d4e5f60 gpu-borrower
allow=scripts 0c1d2e3f4a5b6c7d8e9fa0b1c2d3e4f5 ci
4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e                             # no options: full access
```

A bare token has full access, so a file holding a single token keeps working; scoped tokens are recommended. Leading fields that start with `allow=` are options, so a token may contain `=` (base64) but may not start with `allow=`. Tokens cannot contain blanks. Each line is one token (before scopes, a file's whitespace was stripped and the whole file was one token). Errors name the file and line, and `rpc-serve` refuses to start.

## TLS
`--tls-cert cert.pem --tls-key key.pem` (TLS ≥ 1.2). Clients must verify the server with a CA file (chain plus host-name check) or a pinned SHA-256 certificate fingerprint; there is no "trust anything" setting in the environment or `forge::remote` options.

Build: OpenSSL is found by CMake (`-DOPENSSL_ROOT_DIR=…` if it is outside the toolchain sysroot); `-DFORGE_TLS=OFF` builds without it. Without TLS support, a client that asks for TLS fails with "this build has no TLS". TLS and plain builds of `rpc.hpp` use different inline namespaces, so both can be linked into one program.

### Client certificates (mTLS)
`--tls-client-ca ca.pem` makes the TLS handshake fail unless the client presents a certificate that chains to `ca.pem`. On its own this only gates the connection. Clients present a certificate with `FORGE_RPC_TLS_CERT` and `FORGE_RPC_TLS_KEY`, or the `forge::remote` options `tls_cert` / `tls_key`.

### Client-certificate map
`--tls-client-map FILE` maps verified client certificates to permissions. One rule per line, same options as above:

```
# [allow=PERM[,PERM...]] sha256:<fingerprint> | cn:<common name> [comment]
allow=device sha256:3A:1F:...:9C laptop     # `openssl x509 -noout -fingerprint -sha256 -in cert.pem`; colons optional
cn:ci-runner                                # no options: all
```

- A certificate matches its `sha256:` rule (SHA-256 of the DER certificate) first, else the `cn:` rule for its subject common name. A certificate with no CN or more than one CN matches only by fingerprint. Fingerprints are the robust choice: any certificate the CA issues with the same CN matches a `cn:` rule.
- A certificate that matches no rule is refused (`permission denied: client certificate not authorized`), with or without a token or key. A map with no rules, a duplicate rule, a bad fingerprint or an unknown option make `rpc-serve` refuse to start.
- **With tokens or keys configured**, every client still needs one, and a request must be allowed by both: the permissions are the intersection of the token's or key's and the certificate's. `allow=device` on a certificate keeps that machine off remote scripts whatever token it holds.
- **With only a client map** (no tokens or keys), a mapped certificate authenticates by itself, for device ops only. Remote scripts always need a token or key, and `--allow-scripts` still refuses to start without them.

The server log names the principal of each connection, for example `authenticated token#2 (ci) cert:cn=ci-runner allow=device` (tokens by position in the configuration and name, keys by fingerprint prefix and comment, certificates by fingerprint prefix or CN).

### Channel binding
Over TLS, token and key authentication are bound to the TLS connection (methods 3 and 4), so an exchange captured or relayed by a man in the middle (for example one holding a certificate the client wrongly trusts) fails on any other connection. The binding `cb` is RFC 9266 `tls-exporter` (32 bytes, label `EXPORTER-Channel-Binding`, no context) on TLS 1.3 and on TLS 1.2 with extended master secret, otherwise RFC 5929 `tls-unique`.

A TLS server refuses methods 1 and 2 ("TLS connections need channel-bound authentication"), so **clients built before channel binding cannot authenticate to a TLS server**. A plain TCP server refuses methods 3 and 4. Without TLS the handshake is unchanged.

## Client settings
Device clients (`rpc:host:port`) read the environment. `forge::remote` reads it too, and its options override it.

| Variable | `forge::remote` option | Meaning |
|---|---|---|
| `FORGE_RPC_TOKEN` | `token` | Bearer token |
| `FORGE_RPC_KEY` | `key` | Secret key file (`forge keygen`) |
| `FORGE_RPC_TLS=1` | `tls` | Use TLS |
| `FORGE_RPC_CA` | `ca` | CA file for server verification (turns TLS on) |
| `FORGE_RPC_FINGERPRINT` | `fingerprint` | SHA-256 of the server certificate, hex (turns TLS on) |
| `FORGE_RPC_SERVER_NAME` | `server_name` | Name checked against the certificate (default: the host) |
| `FORGE_RPC_TLS_CERT`, `FORGE_RPC_TLS_KEY` | `tls_cert`, `tls_key` | Client certificate and key, PEM (the certificate turns TLS on) |

## Remote scripts
With `--allow-scripts`, each `FRPS` frame runs in its own child process (`forge script --rpc-child`, source on stdin) with the timeout and memory limit above, at most 32 MiB of output, and `FORGE_RPC_TOKEN`, `FORGE_RPC_KEY`, `FORGE_RPC_TLS_KEY` and `FORGE_SCRIPT_CACHE_SIGN_SEED` removed from its environment. When any `--script-no-*` flag is set, the child applies the OS sandbox with `require` on POSIX, so a sandbox failure fails the script instead of running it unconfined. On Windows it is `best-effort` (job object; the interpreter enforces the rest); see [scripting.md](scripting.md).

```cpp
auto r = forge::remote("box2:7000", "std::println(\"{}\", os.hostname());",
                       {{"key", "~/.forge/me.key"}, {"fingerprint", "ab12…"}, {"args", std::vector<std::string>{"x"}}});
if (r["ok"]) std::print("{}", r["output"]); else std::println(stderr, "{}: {}", r["error_type"], r["error"]);
```

Options: `args`, `timeout_ms`, `name`, plus the credential options in the table above. Failed authentication and refused permissions raise `PermissionError`; network failures raise `IOError`.

`forge::remote_call(node, fn, {args...}, opts)` sends a function instead of a whole script: its source and the plain values it captures travel as a generated script over the same path (same child, sandbox and limits), and the call returns `fn(args...)`. See [scripting.md](scripting.md), "Remote execution".

## Example

```sh
forge keygen -o me                                     # me.key / me.pub
forge rpc-serve --bind 0.0.0.0 --authorized-keys keys.txt --tls-cert c.pem --tls-key k.pem \
    --tls-client-ca clients-ca.pem --allow-scripts
FORGE_RPC_KEY=me.key FORGE_RPC_CA=c.pem FORGE_RPC_TLS_CERT=me-client.pem FORGE_RPC_TLS_KEY=me-client.key \
    FORGE_DEVICE=rpc:box2:7000 forge run model.fpk --prompt hi
```

## Limits
- Not optimised: weights travel with every op (no remote residency), and device ops on one server run one at a time.
- Permissions are coarse: `device` (all ops) and `scripts`. There is no per-op, per-model or read-only scope.
- Client-certificate rules match the SHA-256 fingerprint or subject CN, not SANs or other subject fields; there is no revocation check beyond replacing the CA or removing the rule.
- Only source travels for scripts and `remote_call`; compiled lambdas are not shipped.
- `test_rpc_tls` needs a TLS build and the OpenSSL CLI (`openssl` on `PATH`, `FORGE_TEST_OPENSSL`, or on Windows Git for Windows' copy), and exits 77 (skipped) without them.
