#!/usr/bin/env python3
"""
Golden corpus regression test for retdec-decompiler.

Usage (CTest):
  python3 corpus_regression_test.py <decompiler> <manifest.yaml> <binary> [work_dir]

Checks per fixture:
  - decompiler exit 0
  - output .c non-empty
  - function count >= min_functions
  - optional keyword present in output
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List


def _load_manifest(path: Path) -> List[Dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() in {".yaml", ".yml"}:
        try:
            import yaml  # type: ignore
        except ImportError:
            print("PyYAML required for YAML manifests", file=sys.stderr)
            sys.exit(2)
        data = yaml.safe_load(text)
    else:
        data = json.loads(text)
    fixtures = data.get("fixtures") if isinstance(data, dict) else data
    if not isinstance(fixtures, list):
        print(f"invalid manifest: {path}", file=sys.stderr)
        sys.exit(2)
    return fixtures


def _count_functions_heuristic(code: str) -> int:
    count = 0
    for line in code.splitlines():
        stripped = line.strip()
        if stripped.startswith("//"):
            continue
        if ") {" in stripped or ")\n{" in stripped:
            count += 1
    return count


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "Usage: corpus_regression_test.py <decompiler> <manifest> <binary> [work_dir]",
            file=sys.stderr,
        )
        return 2

    decompiler = Path(sys.argv[1]).resolve()
    manifest_path = Path(sys.argv[2]).resolve()
    binary = Path(sys.argv[3]).resolve()
    work_dir = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else manifest_path.parent

    if not decompiler.is_file():
        print(f"decompiler not found: {decompiler}", file=sys.stderr)
        return 2
    if not binary.is_file():
        print(f"binary not found: {binary}", file=sys.stderr)
        return 2

    fixtures = _load_manifest(manifest_path)
    if not fixtures:
        print("no fixtures in manifest", file=sys.stderr)
        return 2

    work_dir.mkdir(parents=True, exist_ok=True)
    failures = 0

    for fx in fixtures:
        fx_id = str(fx.get("id") or "fixture")
        out_c = work_dir / f"corpus_{fx_id}.c"
        min_funcs = int(fx.get("min_functions") or 1)
        keyword = fx.get("keyword")

        print(f"[corpus] {fx_id}: decompiling {binary.name} -> {out_c.name}")
        try:
            proc = subprocess.run(
                [str(decompiler), "-o", str(out_c), str(binary)],
                capture_output=True,
                text=True,
                timeout=180,
            )
        except subprocess.TimeoutExpired:
            print(f"  FAIL: timeout", file=sys.stderr)
            failures += 1
            continue

        if proc.returncode != 0:
            print(f"  FAIL: exit {proc.returncode}", file=sys.stderr)
            if proc.stderr:
                print(proc.stderr[-1500:], file=sys.stderr)
            failures += 1
            continue

        if not out_c.is_file() or out_c.stat().st_size == 0:
            print("  FAIL: empty output", file=sys.stderr)
            failures += 1
            continue

        code = out_c.read_text(encoding="utf-8", errors="replace")
        fn_count = _count_functions_heuristic(code)
        if fn_count < min_funcs:
            print(
                f"  FAIL: function count {fn_count} < min {min_funcs}",
                file=sys.stderr,
            )
            failures += 1
            continue

        if keyword and str(keyword) not in code:
            print(f"  FAIL: keyword {keyword!r} not in output", file=sys.stderr)
            failures += 1
            continue

        print(f"  OK: {out_c.stat().st_size} bytes, ~{fn_count} functions")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
