#!/usr/bin/env python3
"""Copy real corpus binaries (dereference WSL/OneDrive links) for Docker mounts."""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
dest = root / "build" / "stock-docker-work" / "corpus"
out = root / "build" / "stock-docker-work" / "out"
if dest.exists():
    shutil.rmtree(dest)
dest.mkdir(parents=True, exist_ok=True)
out.mkdir(parents=True, exist_ok=True)

manifest_path = root / "tests" / "decompilebench" / "corpus" / "manifest.json"
items = json.loads(manifest_path.read_text(encoding="utf-8"))
copied = 0
for item in items:
    name = item["name"]
    candidates = [
        Path(item.get("path") or ""),
        root / "tests" / "algorithm_recovery" / "corpus" / name,
        (root / "tests" / "decompilebench" / "corpus" / name),
    ]
    src = None
    for cand in candidates:
        try:
            resolved = cand.resolve()
        except OSError:
            continue
        if resolved.is_file() and resolved.stat().st_size > 0:
            src = resolved
            break
    if src is None:
        print(f"MISSING {name}", file=sys.stderr)
        continue
    target = dest / name
    if target.exists() or target.is_symlink():
        target.unlink()
    shutil.copy2(src, target)
    print(f"{name} {src.stat().st_size}")
    copied += 1
print(f"copied {copied}")
if copied == 0:
    raise SystemExit(2)
