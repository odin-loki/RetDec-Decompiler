# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 2.0.x   | Yes       |
| < 2.0   | No        |

Security fixes land on `main` and the latest `v2.0.x` tag (currently
[`v2.0.24`](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.24)).

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

**Decompiling untrusted binaries is the point of this tool.** The input
file is fully attacker-controlled (PE/ELF/Mach-O, managed bytecode,
archives, optional GGUF weights). Parsers, the unpacker, Capstone
lifting, YARA, and the GUI's `retdec-decompiler` child all run **in
process as the analyst user**. Isolation is operator-provided (VM or
container), not built into RetDec. See [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md).

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
  That is normal use, not an edge case.
- Treat decompiler output, temporary files, and GGUF models as untrusted.
  Recovered C is data. Optional neural refinement never **executes**
  decompiled C; the compile gate is `cc`/`gcc -fsyntax-only`.
- Use `RETDEC_NO_NETWORK=1` for air-gapped analysis; neural refinement
  uses local llama.cpp only when `RETDEC_ENABLE_LLAMACPP` was built and
  `RETDEC_NEURAL_REFINE=1`.
- Release blobs on GitHub may carry keyless Sigstore
  `.sigstore.json` bundles. Authenticode is not applied. macOS
  `RetDec.app` is ad-hoc signed, not notarised.
- Commercial packages must not include GPL-2.0 `capstone2llvmirtool`
  (see LICENSE-COMMERCIAL).

## FIPS

This tree bundles OpenSSL 3.5.8 (`deps/openssl`). The default configure
does **not** enable the OpenSSL FIPS provider. Shipped binaries are **not**
FIPS 140-3 validated. Do not treat `libcrypto` here as a FIPS module.

## PGP

Contact the maintainer for a PGP key if required for encrypted reports. No
public key is published in this repository.

Copyright (c) 2025-2026 Odin Loch, trading as Imortek.
