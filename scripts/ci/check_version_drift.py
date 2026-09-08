#!/usr/bin/env python3
"""REL-04 — CMakeLists.txt, releases/VERSION, CHANGELOG and the workflow tags match.

The first three were checked and the workflows were not, which is where the
drift actually bites. Four release-follow-up workflows trigger on
`push: branches: [main]` with a paths filter, and on that path the tag
resolution falls through to a literal `TAG="v2.0.21"`. That matches the tree
today, so the bug is latent -- but after the next bump, editing
Dockerfile.runtime or scripts/make-appimage.sh on main packs the stale tarball
and docker-from-release.yml retags `ghcr.io/<owner>/retdec:latest` to the old
image. This check exists for exactly that class and could not see it.

Usage:
    python3 scripts/ci/check_version_drift.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE = REPO_ROOT / "CMakeLists.txt"
RELEASES = REPO_ROOT / "releases" / "VERSION"
CHANGELOG = REPO_ROOT / "CHANGELOG.md"
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

# A v-prefixed release tag written out in a workflow. Comments and URLs are
# stripped first: a version named in prose does not count, and neither does a
# third-party pin such as the EnVar NSIS plugin's v0.3.1 download URL.
WORKFLOW_TAG_RE = re.compile(r"v([0-9]+\.[0-9]+\.[0-9]+)")
URL_RE = re.compile(r"https?://\S+")

CMAKE_RE = re.compile(r"^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$", re.MULTILINE)
CHANGELOG_RE = re.compile(r"^## \[([0-9]+\.[0-9]+\.[0-9]+)\]", re.MULTILINE)
RELEASES_RE = re.compile(r"^version=([0-9]+\.[0-9]+\.[0-9]+)\s*$", re.MULTILINE)


def main() -> int:
    cmake_text = CMAKE.read_text(encoding="utf-8", errors="replace")
    rel_text = RELEASES.read_text(encoding="utf-8-sig", errors="replace")
    log_text = CHANGELOG.read_text(encoding="utf-8", errors="replace")

    cmake_m = CMAKE_RE.search(cmake_text)
    rel_m = RELEASES_RE.search(rel_text)
    log_m = CHANGELOG_RE.search(log_text)

    errors: list[str] = []
    if not cmake_m:
        errors.append("CMakeLists.txt: no project VERSION x.y.z")
    if not rel_m:
        errors.append("releases/VERSION: no version=x.y.z")
    if not log_m:
        errors.append("CHANGELOG.md: no ## [x.y.z] heading")

    cmake_v = cmake_m.group(1) if cmake_m else None
    rel_v = rel_m.group(1) if rel_m else None
    log_v = log_m.group(1) if log_m else None
    print(f"check_version_drift: cmake={cmake_v} releases={rel_v} changelog={log_v}")

    if cmake_v and rel_v and cmake_v != rel_v:
        errors.append(f"CMakeLists.txt VERSION {cmake_v} != releases/VERSION {rel_v}")
    if cmake_v and log_v and cmake_v != log_v:
        errors.append(f"CMakeLists.txt VERSION {cmake_v} != CHANGELOG {log_v}")
    if rel_v and log_v and rel_v != log_v:
        errors.append(f"releases/VERSION {rel_v} != CHANGELOG {log_v}")

    # Workflow literals. A tag written into a workflow is a release the job
    # will act on, so it has to be the release this tree is.
    stale: list[str] = []
    scanned = 0
    if rel_v and WORKFLOWS.is_dir():
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for lineno, line in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
            ):
                # Drop comments: a workflow header may legitimately describe a
                # past release ("artefacts uploaded from the v2.0.21 tag").
                code = URL_RE.sub("", line.split("#", 1)[0])
                for m in WORKFLOW_TAG_RE.finditer(code):
                    scanned += 1
                    if m.group(1) != rel_v:
                        stale.append(
                            f"{path.relative_to(REPO_ROOT)}:{lineno}: "
                            f"v{m.group(1)} != releases/VERSION {rel_v}"
                        )
    print(f"check_version_drift: scanned {scanned} workflow tag literal(s)")
    errors.extend(stale)

    if errors:
        print("check_version_drift: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1
    print("check_version_drift: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
