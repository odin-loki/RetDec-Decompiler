#!/usr/bin/env python3
"""DEPS-01 -- a dependency URL is either mirrored or on a host that does not go down.

Why this exists
---------------
A ctest-windows run died ten seconds into the build:

    SHA256 hash of .../zlib-1.3.1.tar.gz does not match expected value
      expected: '9a93b2b7...'
        actual: 'e21df9a9...'

three times over, from zlib.net. The hash check did its job -- it refused
whatever that was -- and then the build had nowhere to go, because ZLIB_URL was
one URL. Every other dependency in cmake/deps.cmake is a GitHub release asset
or tag archive: immutable, CDN-served, and not something one origin server can
take out. zlib was the exception, and the exception is what broke.

The rule
--------
Each `set(<NAME>_URL ...)` in cmake/deps.cmake holds either

  * more than one URL (a list ExternalProject tries in turn), or
  * URLs all on a host in ALLOWED_SOLE_HOSTS.

A single URL on any other host fails, naming it and saying to add a mirror.

What this does NOT check: that the mirrors serve the same bytes. That is what
the URL_HASH beside them is for, and it is the reason a mirror list is safe to
add at all -- a bad mirror fails the hash rather than getting built.

Usage:
    python3 scripts/ci/check_dependency_urls.py [--self-test]
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]
DEPS = ROOT / "cmake" / "deps.cmake"

# Hosts where a single URL is enough: immutable artefacts behind a CDN.
ALLOWED_SOLE_HOSTS = {"github.com", "objects.githubusercontent.com"}

SET_URL = re.compile(
    r"^[ \t]*set\(\s*([A-Z0-9_]+_URL)\s*\n(.*?)^\s*\)\s*$",
    re.MULTILINE | re.DOTALL)
URL_IN = re.compile(r'"([^"]*)"')


def strip_comments(text: str) -> str:
    """Blank out # comments, keeping line numbers, so prose URLs do not count."""
    out = []
    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        out.append("\n" if stripped.startswith("#") else line)
    return "".join(out)


def audit(text: str):
    """[(variable, [urls], ok, why)] for each <NAME>_URL in the file."""
    rows = []
    for m in SET_URL.finditer(strip_comments(text)):
        var, body = m.group(1), m.group(2)
        urls: list[str] = []
        for chunk in URL_IN.findall(body):
            urls.extend(u for u in chunk.split(";") if u.strip())
        urls = [u for u in urls if u.startswith(("http://", "https://"))]
        if not urls:
            continue
        if len(urls) > 1:
            rows.append((var, urls, True, "mirrored"))
            continue
        host = urlparse(urls[0]).netloc.lower()
        ok = host in ALLOWED_SOLE_HOSTS
        rows.append((var, urls, ok, host))
    return rows


def self_test() -> None:
    one_bad = 'set(ZLIB_URL\n\t"https://zlib.net/fossils/zlib-1.3.1.tar.gz"\n\tCACHE STRING ""\n)\n'
    rows = audit(one_bad)
    assert rows and rows[0][0] == "ZLIB_URL" and not rows[0][2], rows

    mirrored = ('set(ZLIB_URL\n'
                '\t"https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz;'
                'https://zlib.net/fossils/zlib-1.3.1.tar.gz"\n\tCACHE STRING ""\n)\n')
    rows = audit(mirrored)
    assert rows and rows[0][2] and len(rows[0][1]) == 2, rows

    sole_github = ('set(YARA_URL\n\t"https://github.com/VirusTotal/yara/archive/v4.5.8.zip"\n'
                   '\tCACHE STRING ""\n)\n')
    rows = audit(sole_github)
    assert rows and rows[0][2], rows

    # A URL in a comment is prose, not a dependency.
    commented = ('# see https://example.invalid/thing.tar.gz for why\n'
                 'set(X_URL\n\t"https://github.com/a/b/archive/v1.zip"\n\tCACHE STRING ""\n)\n')
    rows = audit(commented)
    assert len(rows) == 1 and rows[0][1] == ["https://github.com/a/b/archive/v1.zip"], rows
    print("DEPS-01: self-test OK")


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        self_test()

    if not DEPS.is_file():
        print(f"DEPS-01: FAIL {DEPS} is not there", file=sys.stderr)
        return 1

    rows = audit(DEPS.read_text(encoding="utf-8"))
    if not rows:
        print("DEPS-01: FAIL no <NAME>_URL found in cmake/deps.cmake -- the "
              "parser or the file moved", file=sys.stderr)
        return 1

    bad = [r for r in rows if not r[2]]
    if bad:
        print(f"DEPS-01: FAIL {len(bad)} of {len(rows)} dependency URL(s) have "
              f"one source on a host that can take the build down", file=sys.stderr)
        for var, urls, _, host in bad:
            print(f"  {var}  ->  {urls[0]}   (sole source, host {host})",
                  file=sys.stderr)
        print("", file=sys.stderr)
        print("  Add a mirror: the variable takes a ;-separated list and "
              "ExternalProject tries", file=sys.stderr)
        print("  each in turn. The URL_HASH beside it is what makes that safe.",
              file=sys.stderr)
        return 1

    mirrored = sum(1 for r in rows if len(r[1]) > 1)
    print(f"DEPS-01: OK {len(rows)} dependency URL(s): {mirrored} mirrored, "
          f"{len(rows) - mirrored} sole-source on "
          f"{', '.join(sorted(ALLOWED_SOLE_HOSTS))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
