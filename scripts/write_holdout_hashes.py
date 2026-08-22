#!/usr/bin/env python3
"""B11: SHA-256 of B9 adversarial sources (binaries are gitignored)."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tests" / "algorithm_recovery" / "sources" / "adversarial"
OUT = ROOT / "tests" / "algorithm_recovery" / "holdout" / "source-hashes.json"


def main() -> int:
    files = []
    for path in sorted(SRC.glob("*.c")):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        files.append({"path": f"tests/algorithm_recovery/sources/adversarial/{path.name}", "sha256": digest})
    OUT.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "role": "B11 frozen source holdout (adversarial-positive). Binaries are not committed.",
        "n": len(files),
        "files": files,
    }
    OUT.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {OUT} ({len(files)} sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
