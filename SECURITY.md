# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 2.0.x   | Yes       |
| < 2.0   | No        |

Security fixes land on `main` and the latest `v2.0.x` tag (currently
[`v2.0.21`](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.21)).

## Reporting a vulnerability

**Do not open a public GitHub issue for security-sensitive reports.**

Prefer GitHub's **privately report a vulnerability** flow (repository
Security tab). That is enabled on this repo. You can also email
**odin.loch@outlook.com** with:

- Description and impact
- Reproduction steps or proof-of-concept
- Affected component (CLI, GUI, unpacker, file parsers, neural backend, plugins)
- Version or commit hash

**Acknowledgement:** within **5 business days**.  
**Target fix window:** **90 days** for confirmed high/critical issues; coordinated disclosure preferred.

## Scope

In scope:

- Memory corruption in native parsers (PE, ELF, Mach-O, managed formats)
- Unpacker and Capstone lifting boundary crashes
- Arbitrary code execution via malicious binaries or models
- Path traversal in export/install paths

Out of scope:

- Decompiler output quality or semantic incorrectness (not a security boundary)
- Denial of service on pathological multi-gigabyte inputs without a crash (report anyway)

## Operational guidance

- Run RetDec on **untrusted binaries** in isolated VMs or containers.
- Treat decompiler output, temporary files, and GGUF models as untrusted.
- Use `RETDEC_NO_NETWORK=1` for air-gapped analysis; neural refinement uses local llama.cpp only.
- Commercial packages must not include GPL-2.0 `capstone2llvmirtool` (see LICENSE-COMMERCIAL).

## FIPS

This tree bundles OpenSSL 3.2.6 (`deps/openssl`). The default configure
does **not** enable the OpenSSL FIPS provider. Shipped binaries are **not**
FIPS 140-3 validated. Do not treat `libcrypto` here as a FIPS module.

## PGP

Contact the maintainer for a PGP key if required for encrypted reports. No
public key is published in this repository.

Copyright (c) 2025-2026 Odin Loch, trading as Imortek.
