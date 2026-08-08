#!/usr/bin/env python3
"""Smoke test for triton_diff_gate.py stdout mode."""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "triton_diff_gate.py"

SRC = """
#include <stdio.h>
int main(void) { printf("ok\\n"); return 0; }
"""


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        orig = td_path / "orig.c"
        ref = td_path / "ref.c"
        orig.write_text(SRC, encoding="utf-8")
        ref.write_text(SRC, encoding="utf-8")
        proc = subprocess.run(
            [sys.executable, str(GATE), str(orig), str(ref), "--mode", "stdout"],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            print(proc.stdout, proc.stderr, file=sys.stderr)
            return 1
    print("triton_diff_gate smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
