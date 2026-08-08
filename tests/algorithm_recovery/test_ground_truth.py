#!/usr/bin/env python3
"""Tests for generate_ground_truth label key resolution."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "generate_ground_truth.py"


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        base = Path(td)
        sources = base / "sources"
        gen = sources / "generated"
        gen.mkdir(parents=True)
        (sources / "bubblesort.c").write_text("int main(void){return 0;}\n", encoding="utf-8")
        (sources / "bubblesort.labels.json").write_text(
            json.dumps({"algorithms": ["BubbleSort", "Sort"]}),
            encoding="utf-8",
        )
        (gen / "pthread_mutex.c").write_text("int main(void){return 0;}\n", encoding="utf-8")
        (gen / "pthread_mutex.labels.json").write_text(
            json.dumps({"algorithms": ["Mutex", "Concurrency"]}),
            encoding="utf-8",
        )
        manifest = [
            {"name": "bubblesort-gcc-O0", "source": "bubblesort.c"},
            {"name": "generated_pthread_mutex-gcc-O0", "source": "generated/pthread_mutex.c"},
        ]
        manifest_path = base / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        out = base / "corpus.json"
        proc = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--sources",
                str(sources),
                "--manifest",
                str(manifest_path),
                "--out",
                str(out),
            ],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            print(proc.stderr or proc.stdout, file=sys.stderr)
            return 1
        ground = json.loads(out.read_text(encoding="utf-8"))
        if ground.get("bubblesort-gcc-O0") != ["BubbleSort", "Sort"]:
            print(f"bad bubblesort labels: {ground.get('bubblesort-gcc-O0')}", file=sys.stderr)
            return 1
        if ground.get("generated_pthread_mutex-gcc-O0") != ["Mutex", "Concurrency"]:
            print(
                f"bad pthread labels: {ground.get('generated_pthread_mutex-gcc-O0')}",
                file=sys.stderr,
            )
            return 1

    print("generate_ground_truth tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
