#!/usr/bin/env python3
"""LEG-12 — GUI CMake must not request a static Qt package.

The official Qt 6 imported targets (Qt6::Core / Gui / Widgets) are shared
libraries from find_package. A STATIC find_package would produce an LGPL
relink problem. This check does not download a Release zip; dumpbin of
the published portable zip is .github/workflows/qt-lgpl-evidence.yml.

Usage:
    python3 scripts/ci/check_qt_dynamic_link.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE = REPO_ROOT / "src" / "gui" / "CMakeLists.txt"
STATIC_FIND = re.compile(
    r"find_package\s*\(\s*Qt6[^)]*\bSTATIC\b",
    re.IGNORECASE | re.DOTALL,
)


def main() -> int:
    if not CMAKE.is_file():
        print("check_qt_dynamic_link: FAIL (src/gui/CMakeLists.txt missing)")
        return 1
    text = CMAKE.read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    if STATIC_FIND.search(text):
        errors.append("src/gui/CMakeLists.txt find_package(Qt6 … STATIC)")
    if "Qt6::Core" not in text:
        errors.append("src/gui/CMakeLists.txt does not link Qt6::Core")
    if "Qt6::Gui" not in text:
        errors.append("src/gui/CMakeLists.txt does not link Qt6::Gui")
    if "Qt6::Widgets" not in text:
        errors.append("src/gui/CMakeLists.txt does not link Qt6::Widgets")
    if errors:
        print("check_qt_dynamic_link: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1
    print("check_qt_dynamic_link: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
