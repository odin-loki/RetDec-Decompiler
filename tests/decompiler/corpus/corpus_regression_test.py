#!/usr/bin/env python3
"""
Golden corpus regression test for retdec-decompiler.

Usage (CTest):
  python3 corpus_regression_test.py <decompiler> <manifest.yaml> <fixtures_dir> [work_dir]

Checks per fixture:
  - input binary exists (or skip when skip_if_missing)
  - decompiler exit 0
  - output non-empty
  - function count >= min_functions (native heuristic)
  - optional keyword present in output
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional


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


def _resolve_binary(
    fx: Dict[str, Any],
    fixtures_dir: Path,
    manifest_dir: Path,
) -> Optional[Path]:
    raw = fx.get("binary")
    if not raw:
        fx_id = str(fx.get("id") or "fixture")
        raw = fx_id

    rel = Path(str(raw))
    if rel.is_absolute():
        return rel

    candidates = [
        fixtures_dir / rel,
        fixtures_dir / f"{rel.name}{_exe_suffix()}",
        manifest_dir / rel,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def _exe_suffix() -> str:
    return ".exe" if os.name == "nt" else ""


def _output_candidates(work_dir: Path, fx_id: str, output_ext: str) -> List[Path]:
    base = work_dir / f"corpus_{fx_id}"
    ext = output_ext or ".c"
    if not ext.startswith("."):
        ext = "." + ext
    paths = [base.with_suffix(ext)]
    if ext == ".wat":
        paths.append(base.with_name(base.name + ".wat"))
    return paths


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "Usage: corpus_regression_test.py <decompiler> <manifest> <fixtures_dir> [work_dir]",
            file=sys.stderr,
        )
        return 2

    decompiler = Path(sys.argv[1]).resolve()
    manifest_path = Path(sys.argv[2]).resolve()
    fixtures_dir = Path(sys.argv[3]).resolve()
    manifest_dir = manifest_path.parent
    work_dir = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else manifest_dir / "out"

    if not decompiler.is_file():
        print(f"decompiler not found: {decompiler}", file=sys.stderr)
        return 2
    if not fixtures_dir.is_dir():
        print(f"fixtures dir not found: {fixtures_dir}", file=sys.stderr)
        return 2

    fixtures = _load_manifest(manifest_path)
    if not fixtures:
        print("no fixtures in manifest", file=sys.stderr)
        return 2

    work_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    skipped = 0
    ran = 0

    for fx in fixtures:
        fx_id = str(fx.get("id") or "fixture")
        binary = _resolve_binary(fx, fixtures_dir, manifest_dir)
        skip_if_missing = bool(fx.get("skip_if_missing"))

        if binary is None or not binary.is_file():
            msg = f"input missing: {fx.get('binary', fx_id)}"
            if skip_if_missing:
                print(f"[corpus] {fx_id}: SKIP ({msg})")
                skipped += 1
                continue
            print(f"[corpus] {fx_id}: FAIL ({msg})", file=sys.stderr)
            failures += 1
            continue

        fmt = str(fx.get("format") or "native")
        output_ext = str(fx.get("output_ext") or ".c")
        min_funcs = int(fx.get("min_functions") or 1)
        keyword = fx.get("keyword")
        out_paths = _output_candidates(work_dir, fx_id, output_ext)
        out_primary = out_paths[0]

        print(f"[corpus] {fx_id}: decompiling {binary.name} -> {out_primary.name}")
        ran += 1
        try:
            proc = subprocess.run(
                [str(decompiler), "-o", str(out_primary), str(binary)],
                capture_output=True,
                text=True,
                timeout=180,
            )
        except subprocess.TimeoutExpired:
            print("  FAIL: timeout", file=sys.stderr)
            failures += 1
            continue

        if proc.returncode != 0:
            print(f"  FAIL: exit {proc.returncode}", file=sys.stderr)
            if proc.stderr:
                print(proc.stderr[-1500:], file=sys.stderr)
            failures += 1
            continue

        out_file = next((p for p in out_paths if p.is_file() and p.stat().st_size > 0), None)
        if out_file is None:
            print("  FAIL: empty output", file=sys.stderr)
            failures += 1
            continue

        code = out_file.read_text(encoding="utf-8", errors="replace")
        fn_count = _count_functions_heuristic(code) if fmt == "native" else 0
        if fmt == "native" and fn_count < min_funcs:
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

        detail = f"{out_file.stat().st_size} bytes"
        if fmt == "native":
            detail += f", ~{fn_count} functions"
        print(f"  OK: {detail}")

    if ran == 0 and skipped == len(fixtures):
        print("all fixtures skipped", file=sys.stderr)
        return 2

    print(f"\nSummary: {ran} ran, {skipped} skipped, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
