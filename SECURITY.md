# Security Policy

## Supported versions

Security fixes are applied to the **latest release** on the default branch. Older tagged releases may not receive backports unless agreed with the maintainer.

## Reporting a vulnerability

**Do not open a public GitHub issue for security-sensitive reports.**

Send details to **odin.loch@outlook.com** with:

- A description of the issue and impact
- Steps to reproduce or a proof-of-concept (if available)
- Affected component (CLI, GUI, unpacker, plugin loader, etc.)
- RetDec version or commit hash

You should receive an acknowledgement within **5 business days**. We will work with you on a fix and coordinated disclosure timeline.

## Scope notes

RetDec decompiles **untrusted binaries**. Treat decompiler output, temporary files, and plugin-loaded code as potentially hostile. Run in isolated environments when analysing malware.

Copyright (c) 2025-2026 Odin Loch, trading as Imortek.
