#!/usr/bin/env python3
"""MAC-01 -- retdec-gui.app must resolve and verify, or it is not shippable.

What was wrong
--------------
The first macOS build of this tree assembled `retdec-gui.app` and macdeployqt
reported, on every run:

    ERROR: no file at "/opt/homebrew/opt/webp/lib/libwebp.7.dylib"
    ERROR: no file at "/opt/homebrew/opt/webp/lib/libsharpyuv.0.dylib"
    ERROR: no file at "/opt/homebrew/opt/brotli/lib/libbrotlicommon.1.dylib"
    ...
    retdec-gui.app: code object is not signed at all
    codesign verification error

It did not fail the build, and nothing depended on the bundle, so it was
recorded and left. The moment there is a macOS installer job that ships the
bundle, a bundle whose libraries live in someone else's Homebrew prefix and
whose signature does not verify stops being cosmetic: it is an app that does
not open on any machine but the one that built it.

What this does
--------------
Walks every Mach-O file in the bundle and follows its load commands.

  * `/usr/lib/...` and `/System/...` are the OS and stay where they are.
  * Anything else -- an absolute path into a Homebrew prefix, or an `@rpath`
    that resolves outside the bundle -- is copied into `Contents/Frameworks`,
    the referring file is rewritten to point at `@rpath/<name>`, and the
    referring file gets an `LC_RPATH` that reaches `Contents/Frameworks` from
    where it sits. Copied libraries are walked in turn, so a dependency three
    deep comes along with the two above it.
  * Everything touched is re-signed ad-hoc, innermost first, and the bundle
    last. `codesign --verify --deep --strict` then has to pass.

With `--fix` it repairs and then verifies. Without, it only verifies, which is
what a release job wants after the repair has already happened.

Usage:
    python3 scripts/ci/check_macos_bundle.py [--fix] path/to/Foo.app
    python3 scripts/ci/check_macos_bundle.py --self-test
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# The two prefixes whose libraries belong to the operating system and are
# present on every machine. Everything else has to travel with the bundle.
SYSTEM_PREFIXES = ("/usr/lib/", "/System/")

MACHO_MAGIC = {
    b"\xcf\xfa\xed\xfe",  # 64-bit little endian
    b"\xce\xfa\xed\xfe",  # 32-bit little endian
    b"\xca\xfe\xba\xbe",  # fat
    b"\xbe\xba\xfe\xca",  # fat, other endianness
}


def run(cmd: list[str]) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


# ---------------------------------------------------------------- parsing ---
# Kept free of subprocess and the filesystem so the self-test can drive them
# with recorded tool output on any platform.

OTOOL_DEP_RE = re.compile(r"^\s+(\S+)\s+\(compatibility version")


def parse_otool_L(out: str, is_dylib_id_first: bool = True) -> list[str]:
    """Dependencies from `otool -L`. The id line of a dylib is not one."""
    lines = [l for l in out.splitlines() if OTOOL_DEP_RE.match(l)]
    deps = [OTOOL_DEP_RE.match(l).group(1) for l in lines]
    return deps


def parse_otool_rpaths(out: str) -> list[str]:
    """LC_RPATH paths from `otool -l`."""
    rpaths: list[str] = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" not in line:
            continue
        for j in range(i + 1, min(i + 5, len(lines))):
            m = re.search(r"^\s*path\s+(.+?)\s+\(offset \d+\)\s*$", lines[j])
            if m:
                rpaths.append(m.group(1))
                break
    return rpaths


def _norm(p: Path) -> Path:
    """Collapse `..` lexically. resolve() would follow symlinks, and a
    .framework is symlinks all the way down -- Versions/Current -> A."""
    return Path(os.path.normpath(str(p)))


def expand(path: str, holder: Path, bundle: Path, rpaths: list[str]) -> list[Path]:
    """Every filesystem path a load-command string could mean, in order."""
    exe_dir = bundle / "Contents" / "MacOS"
    if path.startswith("@loader_path/"):
        return [_norm(holder.parent / path[len("@loader_path/"):])]
    if path.startswith("@executable_path/"):
        return [_norm(exe_dir / path[len("@executable_path/"):])]
    if path.startswith("@rpath/"):
        tail = path[len("@rpath/"):]
        out: list[Path] = []
        for r in rpaths:
            if r.startswith("@loader_path"):
                base = holder.parent / r[len("@loader_path"):].lstrip("/")
            elif r.startswith("@executable_path"):
                base = exe_dir / r[len("@executable_path"):].lstrip("/")
            else:
                base = Path(r)
            out.append(_norm(base / tail))
        return out
    return [Path(path)]


# ------------------------------------------------------------ filesystem ---

def is_macho(p: Path) -> bool:
    if p.is_symlink() or not p.is_file():
        return False
    try:
        with p.open("rb") as fh:
            return fh.read(4) in MACHO_MAGIC
    except OSError:
        return False


def macho_files(bundle: Path) -> list[Path]:
    return sorted(p for p in bundle.rglob("*") if is_macho(p))


def deps_of(p: Path) -> list[str]:
    return parse_otool_L(run(["otool", "-L", str(p)]))


def rpaths_of(p: Path) -> list[str]:
    return parse_otool_rpaths(run(["otool", "-l", str(p)]))


def inside(p: Path, bundle: Path) -> bool:
    try:
        p.resolve().relative_to(bundle.resolve())
        return True
    except ValueError:
        return False


def rpath_to_frameworks(holder: Path, bundle: Path) -> str:
    """An @loader_path rpath that reaches Contents/Frameworks from holder."""
    fw = (bundle / "Contents" / "Frameworks").resolve()
    rel = os.path.relpath(fw, holder.resolve().parent)
    return f"@loader_path/{rel}"


class Report:
    def __init__(self) -> None:
        self.copied: list[str] = []
        self.rewritten: list[str] = []
        self.unresolved: list[tuple[str, str]] = []
        self.foreign: list[tuple[str, str]] = []


def walk(bundle: Path, fix: bool) -> Report:
    rep = Report()
    fw = bundle / "Contents" / "Frameworks"
    queue = macho_files(bundle)
    seen: set[Path] = set()

    while queue:
        f = queue.pop(0)
        if f in seen:
            continue
        seen.add(f)
        try:
            deps = deps_of(f)
            rpaths = rpaths_of(f)
        except subprocess.CalledProcessError:
            continue

        own_id = f.name
        for dep in deps:
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            # A dylib's own LC_ID_DYLIB comes back from otool -L too.
            if dep.endswith("/" + own_id) and dep.startswith("@rpath/") and \
                    (fw / own_id).exists() and (fw / own_id).samefile(f):
                continue

            candidates = expand(dep, f, bundle, rpaths)
            resolved = next((c for c in candidates if c.exists()), None)

            if resolved is not None and inside(resolved, bundle):
                continue

            name = Path(dep).name
            target = fw / name

            if resolved is None:
                if target.exists():
                    # It is already here; the referring file just cannot see
                    # it. Give it an rpath that can.
                    if fix:
                        add_rpath(f, rpath_to_frameworks(f, bundle))
                        rep.rewritten.append(f"{f.name}: rpath -> Frameworks for {name}")
                    continue
                rep.unresolved.append((str(f.relative_to(bundle)), dep))
                continue

            # Resolved, but outside the bundle: a Homebrew prefix, an Xcode
            # path, someone's home directory. It has to come along.
            rep.foreign.append((str(f.relative_to(bundle)), dep))
            if not fix:
                continue

            if not target.exists():
                fw.mkdir(parents=True, exist_ok=True)
                shutil.copy2(resolved, target)
                target.chmod(target.stat().st_mode | 0o200)
                run(["install_name_tool", "-id", f"@rpath/{name}", str(target)])
                rep.copied.append(name)
                queue.append(target)

            run(["install_name_tool", "-change", dep, f"@rpath/{name}", str(f)])
            add_rpath(f, rpath_to_frameworks(f, bundle))
            rep.rewritten.append(f"{f.name}: {dep} -> @rpath/{name}")

    return rep


def add_rpath(f: Path, rpath: str) -> None:
    if rpath in rpaths_of(f):
        return
    subprocess.run(["install_name_tool", "-add_rpath", rpath, str(f)],
                   capture_output=True, text=True)


def resign(bundle: Path) -> None:
    """Ad-hoc signature, innermost first; the bundle itself last."""
    files = macho_files(bundle)
    # Deepest paths first so a container is signed after everything it holds.
    for f in sorted(files, key=lambda p: len(p.parts), reverse=True):
        subprocess.run(["codesign", "--force", "--timestamp=none", "--sign", "-",
                        str(f)], capture_output=True, text=True)
    subprocess.run(["codesign", "--force", "--timestamp=none", "--sign", "-",
                    str(bundle)], capture_output=True, text=True)


def verify(bundle: Path) -> tuple[bool, str]:
    p = subprocess.run(["codesign", "--verify", "--deep", "--strict",
                        "--verbose=2", str(bundle)],
                       capture_output=True, text=True)
    return p.returncode == 0, (p.stdout + p.stderr).strip()


def self_test() -> None:
    """The parsers, against tool output recorded from a real bundle."""
    otool_L = """retdec-gui.app/Contents/MacOS/retdec-gui:
\t@rpath/QtWidgets.framework/Versions/A/QtWidgets (compatibility version 6.0.0, current version 6.7.3)
\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1800.101.0)
\t/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (compatibility version 45.0.0, current version 2487.0.0)
"""
    assert parse_otool_L(otool_L) == [
        "@rpath/QtWidgets.framework/Versions/A/QtWidgets",
        "/usr/lib/libc++.1.dylib",
        "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit",
    ], parse_otool_L(otool_L)

    otool_l = """Load command 20
          cmd LC_RPATH
      cmdsize 40
         path @executable_path/../Frameworks (offset 12)
Load command 21
          cmd LC_RPATH
      cmdsize 32
         path /opt/homebrew/lib (offset 12)
"""
    assert parse_otool_rpaths(otool_l) == [
        "@executable_path/../Frameworks", "/opt/homebrew/lib"], parse_otool_rpaths(otool_l)

    bundle = Path("/tmp/x/retdec-gui.app")
    holder = bundle / "Contents" / "PlugIns" / "imageformats" / "libqwebp.dylib"
    # An @rpath dep is looked for under each LC_RPATH of the file that names it.
    got = expand("@rpath/libwebp.7.dylib", holder, bundle,
                 ["@loader_path/../../Frameworks", "/opt/homebrew/opt/webp/lib"])
    assert got == [
        bundle / "Contents" / "Frameworks" / "libwebp.7.dylib",
        Path("/opt/homebrew/opt/webp/lib/libwebp.7.dylib"),
    ], got
    # @executable_path is the bundle's MacOS directory, wherever the holder is.
    assert expand("@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore",
                  holder, bundle, []) == [
        bundle / "Contents" / "Frameworks" /
        "QtCore.framework" / "Versions" / "A" / "QtCore"]
    # An absolute path means itself.
    assert expand("/usr/lib/libz.1.dylib", holder, bundle, []) == \
        [Path("/usr/lib/libz.1.dylib")]

    # The rpath written into a plugin has to climb out to Contents/Frameworks.
    assert rpath_to_frameworks(holder, bundle) == "@loader_path/../../Frameworks"
    assert rpath_to_frameworks(bundle / "Contents" / "MacOS" / "retdec-gui",
                               bundle) == "@loader_path/../Frameworks"
    print("MAC-01: self-test OK")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("bundle", nargs="?")
    ap.add_argument("--fix", action="store_true",
                    help="copy in and rewrite what does not resolve, then verify")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        if not args.bundle:
            return 0

    if not args.bundle:
        print("MAC-01: pass the path to a .app bundle", file=sys.stderr)
        return 2
    if sys.platform != "darwin":
        print("MAC-01: needs otool/install_name_tool/codesign; this is not macOS",
              file=sys.stderr)
        return 2

    bundle = Path(args.bundle)
    if not bundle.is_dir():
        print(f"MAC-01: FAIL {bundle} is not a directory", file=sys.stderr)
        return 1

    rep = walk(bundle, fix=args.fix)
    if args.fix:
        for n in rep.copied:
            print(f"  copied  {n}")
        for n in rep.rewritten:
            print(f"  rewrote {n}")
        resign(bundle)
        # Look again with fresh eyes: the repair has to have actually worked.
        rep = walk(bundle, fix=False)

    ok = True
    if rep.unresolved:
        ok = False
        print(f"MAC-01: FAIL {len(rep.unresolved)} load command(s) resolve to "
              f"nothing", file=sys.stderr)
        for who, dep in rep.unresolved:
            print(f"  {who}  ->  {dep}", file=sys.stderr)
    if rep.foreign:
        ok = False
        print(f"MAC-01: FAIL {len(rep.foreign)} load command(s) point outside "
              f"the bundle", file=sys.stderr)
        for who, dep in rep.foreign:
            print(f"  {who}  ->  {dep}", file=sys.stderr)

    signed, detail = verify(bundle)
    if not signed:
        ok = False
        print(f"MAC-01: FAIL codesign --verify --deep --strict: {detail}",
              file=sys.stderr)

    if not ok:
        return 1
    print(f"MAC-01: OK {bundle.name} resolves inside itself and its signature "
          f"verifies")
    return 0


if __name__ == "__main__":
    sys.exit(main())
