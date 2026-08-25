#!/usr/bin/env python3
"""REL-06 — CycloneDX 1.5 BOM of CMake-pinned download artefacts.

This is the pin set in cmake/deps.cmake (URL + SHA-256), not a full
runtime graph of every transitive system library.

Usage:
    python3 scripts/ci/generate_cyclonedx.py --out retdec.cdx.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE = REPO_ROOT / "CMakeLists.txt"
DEPS = REPO_ROOT / "cmake" / "deps.cmake"

CMAKE_VER_RE = re.compile(r"^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$", re.MULTILINE)
URL_RE = re.compile(
    r"set\(\s*([A-Z0-9_]+)_URL\s+\"([^\"]+)\"",
    re.MULTILINE,
)
SHA_RE = re.compile(
    r"set\(\s*([A-Z0-9_]+)_(?:ARCHIVE_)?SHA256\s+\"([0-9a-fA-F]{64})\"",
    re.MULTILINE,
)


def cmake_version() -> str:
    text = CMAKE.read_text(encoding="utf-8", errors="replace")
    m = CMAKE_VER_RE.search(text)
    if not m:
        raise SystemExit("generate_cyclonedx: no project VERSION in CMakeLists.txt")
    return m.group(1)


def pinned_components() -> list[dict]:
    text = DEPS.read_text(encoding="utf-8", errors="replace")
    urls = {m.group(1): m.group(2) for m in URL_RE.finditer(text)}
    shas = {m.group(1): m.group(2).lower() for m in SHA_RE.finditer(text)}
    components: list[dict] = []
    for name in sorted(urls):
        url = urls[name]
        component: dict = {
            "type": "library",
            "name": name.lower().replace("_", "-"),
            "bom-ref": name.lower(),
            "description": f"CMake pin {name}_URL",
            "externalReferences": [{"type": "distribution", "url": url}],
        }
        sha = shas.get(name)
        if sha:
            component["hashes"] = [{"alg": "SHA-256", "content": sha}]
        components.append(component)
    if not components:
        raise SystemExit("generate_cyclonedx: no *_URL pins in cmake/deps.cmake")
    return components


def bom_document() -> dict:
    version = cmake_version()
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "retdec",
                "version": version,
                "bom-ref": f"retdec@{version}",
            }
        },
        "components": pinned_components(),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write JSON to this path (stdout if omitted)")
    args = ap.parse_args()
    doc = bom_document()
    payload = json.dumps(doc, indent=2) + "\n"
    if args.out:
        Path(args.out).write_text(payload, encoding="utf-8")
        print(f"generate_cyclonedx: wrote {args.out} ({len(doc['components'])} components)")
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
