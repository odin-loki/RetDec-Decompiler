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
# The name group is lazy on purpose. Greedy, `([A-Z0-9_]+)` swallows the
# ARCHIVE segment before the optional group can match it, so
# `set(CAPSTONE_ARCHIVE_SHA256 ...)` was captured under the name
# CAPSTONE_ARCHIVE while URL_RE captured CAPSTONE -- shas.get(name) missed for
# every dependency written that way, and the SBOM this generates went out with
# a hash for one component of twelve. It is cosign-signed and attached to every
# release, so an SBOM that pins nothing is worse than no SBOM.
SHA_RE = re.compile(
    r"set\(\s*([A-Z0-9_]+?)_(?:ARCHIVE_)?SHA256\b\s+\"([0-9a-fA-F]{64})\"",
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

    # A pin that deps.cmake gives a SHA-256 must carry it here. Silence was
    # how the regex above went unnoticed: the generator exited 0 and the
    # workflow step passed while the document pinned nothing.
    unhashed = [name for name in sorted(urls) if name in shas and not shas.get(name)]
    dropped = sorted(set(shas) - set(urls))
    missing = [name for name in sorted(urls) if name not in shas]
    if dropped:
        raise SystemExit(
            "generate_cyclonedx: deps.cmake pins a SHA-256 for "
            + ", ".join(dropped)
            + " but no matching *_URL was found -- the two regexes disagree"
        )
    if unhashed:
        raise SystemExit("generate_cyclonedx: empty SHA-256 for " + ", ".join(unhashed))
    if missing:
        print(
            "generate_cyclonedx: no SHA-256 pinned for " + ", ".join(missing),
            file=sys.stderr,
        )
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
