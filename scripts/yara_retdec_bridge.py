#!/usr/bin/env python3
"""Bridge YARA scan hits to suggested retdec-decompiler commands.

If the ``yara`` Python module or ``yara`` CLI is unavailable, exits cleanly
with a skip message (exit code 0).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


def find_yara_cli() -> Optional[str]:
    for name in ("yara64", "yara"):
        path = shutil.which(name)
        if path:
            return path
    return None


def try_import_yara():
    try:
        import yara  # type: ignore

        return yara
    except ImportError:
        return None


def iter_scan_files(directory: Path) -> List[Path]:
    files: List[Path] = []
    for root, _dirs, names in os.walk(directory):
        for name in names:
            p = Path(root) / name
            if p.is_file():
                files.append(p)
    return files


def scan_with_python(rule_path: Path, targets: Sequence[Path]) -> List[Tuple[Path, str]]:
    yara = try_import_yara()
    if yara is None:
        return []
    rules = yara.compile(filepath=str(rule_path))
    hits: List[Tuple[Path, str]] = []
    for target in targets:
        try:
            matches = rules.match(str(target))
        except yara.Error:
            continue
        for match in matches:
            hits.append((target, match.rule))
    return hits


def scan_with_cli(rule_path: Path, targets: Sequence[Path], yara_exe: str) -> List[Tuple[Path, str]]:
    hits: List[Tuple[Path, str]] = []
    for target in targets:
        try:
            proc = subprocess.run(
                [yara_exe, str(rule_path), str(target)],
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        for line in proc.stdout.splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2:
                hits.append((Path(parts[1]), parts[0]))
            elif len(parts) == 1 and proc.returncode == 0:
                hits.append((target, parts[0]))
    return hits


def suggest_decompiler_command(match_path: Path, decompiler: str) -> str:
    return f'{decompiler} -o "{match_path.with_suffix(".c")}" "{match_path}"'


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run YARA rules and print suggested retdec-decompiler commands.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("rule", type=Path, help="YARA rule file (.yar / .yara)")
    parser.add_argument("directory", type=Path, help="Directory to scan recursively")
    parser.add_argument(
        "--decompiler",
        default="retdec-decompiler",
        help="Decompiler executable name or path for suggestions",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Max matches to print (0 = unlimited)",
    )
    return parser.parse_args(list(argv) if argv is not None else None)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    if not args.rule.is_file():
        print(f"Error: rule file not found: {args.rule}", file=sys.stderr)
        return 1
    if not args.directory.is_dir():
        print(f"Error: scan directory not found: {args.directory}", file=sys.stderr)
        return 1

    targets = iter_scan_files(args.directory)
    if not targets:
        print(f"No files to scan under {args.directory}", file=sys.stderr)
        return 0

    hits = scan_with_python(args.rule, targets)
    backend = "python-yara"
    if not hits:
        cli = find_yara_cli()
        if cli:
            hits = scan_with_cli(args.rule, targets, cli)
            backend = cli
        else:
            print(
                "YARA not available (install `yara-python` or put `yara` on PATH); skipping scan.",
                file=sys.stderr,
            )
            return 0

    print(f"# YARA backend: {backend}", file=sys.stderr)
    printed = 0
    seen = set()
    for match_path, rule_name in hits:
        key = (str(match_path.resolve()), rule_name)
        if key in seen:
            continue
        seen.add(key)
        cmd = suggest_decompiler_command(match_path, args.decompiler)
        print(f"# match: {rule_name} -> {match_path}")
        print(cmd)
        printed += 1
        if args.limit and printed >= args.limit:
            break
    if printed == 0:
        print("No YARA matches.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
