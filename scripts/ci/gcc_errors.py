#!/usr/bin/env python3
import json
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
art = root / "tests/decompilebench/artifacts/fork"
out = root / "results/gcc-syntax-errors.txt"
lines = []
for path in sorted(art.glob("*.c")):
    if path.name.endswith(".buildable.c") or path.name.endswith(".refined.c"):
        kind = "sidecar"
    else:
        kind = "raw"
    proc = subprocess.run(
        ["gcc", "-fsyntax-only", "-std=gnu11", "-w", str(path)],
        capture_output=True,
        text=True,
    )
    err = (proc.stderr or proc.stdout or "").strip()
    lines.append(f"===== {kind} {path.name} exit={proc.returncode} =====")
    lines.append("\n".join(err.splitlines()[:25]) if err else "(no diagnostics)")
    lines.append("")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"Wrote {out} ({len(lines)} lines)")

payload = json.loads((root / "results/decompilebench-ci-core.json").read_text(encoding="utf-8"))
print("summary:", json.dumps(payload.get("summary"), indent=2))
print("compare:", json.dumps(payload.get("compare"), indent=2))
