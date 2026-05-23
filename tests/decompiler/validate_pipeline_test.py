#!/usr/bin/env python3
"""Validate decompile profile JSON files against pipeline_builder_schema rules."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    validator = repo / "scripts" / "validate_pipeline_json.py"
    if not validator.is_file():
        print(f"Error: missing validator: {validator}", file=sys.stderr)
        return 1

    cmd = [sys.executable, str(validator), "--all-profiles"]
    print("Running:", " ".join(cmd))
    result = subprocess.run(cmd, cwd=str(repo))
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
