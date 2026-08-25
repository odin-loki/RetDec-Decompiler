#!/usr/bin/env python3
"""LEG-11: Keystone-linked capstone2llvmirtool must not be installed.

Fails if src/capstone2llvmirtool/CMakeLists.txt still install()s the tool,
or if packaging/ names it.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE = REPO_ROOT / "src" / "capstone2llvmirtool" / "CMakeLists.txt"
PACKAGING = REPO_ROOT / "packaging"
NEEDLE = "install(TARGETS capstone2llvmirtool"


def main() -> int:
    errors: list[str] = []
    text = CMAKE.read_text(encoding="utf-8") if CMAKE.is_file() else ""
    if NEEDLE in text:
        errors.append(f"{CMAKE.relative_to(REPO_ROOT)} still {NEEDLE}")
    if "retdec::deps::keystone" not in text and "keystone" not in text.lower():
        errors.append("capstone2llvmirtool CMakeLists.txt no longer mentions keystone; update this check")
    if PACKAGING.is_dir():
        for path in PACKAGING.rglob("*"):
            if not path.is_file():
                continue
            try:
                body = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "capstone2llvmirtool" in body:
                errors.append(f"{path.relative_to(REPO_ROOT)} names capstone2llvmirtool")
    if errors:
        print("check_keystone_not_installed: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1
    print("check_keystone_not_installed: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
