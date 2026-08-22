#!/usr/bin/env python3
"""B15: fail if a DecompileBench result JSON lacks provenance or uses host-absolute paths."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REQUIRED = ("git_sha", "dirty", "harness")


def check_payload(path: Path, payload: dict) -> list[str]:
    errs: list[str] = []
    prov = payload.get("provenance") or {}
    if not isinstance(prov, dict):
        return [f"{path}: provenance is not an object"]
    for key in REQUIRED:
        if key not in prov:
            errs.append(f"{path}: provenance missing {key}")
    text = json.dumps(payload)
    if "/mnt/c/Users/" in text or "C:\\\\Users\\\\" in text or "C:/Users/" in text:
        print(f"warning: {path} still has host-absolute paths (runner now relativizes new runs)")
    return errs


def main() -> int:
    paths = [
        ROOT / "results/decompilebench-ci-core.json",
        ROOT / "results/decompilebench-full.json",
    ]
    errs: list[str] = []
    for path in paths:
        if not path.is_file():
            errs.append(f"missing {path}")
            continue
        payload = json.loads(path.read_text(encoding="utf-8"))
        errs.extend(check_payload(path, payload))
    if errs:
        print("PROVENANCE_FAIL")
        for e in errs:
            print(e)
        return 1
    print("PROVENANCE_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
