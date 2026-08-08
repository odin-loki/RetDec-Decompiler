# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | Yes       |
| < 1.0   | No        |

Security fixes land on `main` and the latest `v1.0.x` tag.

## Reporting a vulnerability

**Do not open a public GitHub issue for security-sensitive reports.**

Email **odin.loch@outlook.com** with:

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

## PGP

Contact the maintainer for a PGP key if required for encrypted reports.

Copyright (c) 2025-2026 Odin Loch, trading as Imortek.
