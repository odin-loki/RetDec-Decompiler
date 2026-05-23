#!/usr/bin/env python3
"""Validate RetDec pipeline JSON against docs/pipeline_builder_schema.json.

Usage:
    python3 scripts/validate_pipeline_json.py FILE [FILE...]
    python3 scripts/validate_pipeline_json.py --all-profiles

Exit 0 if all files validate; exit 1 on error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable, List, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = REPO_ROOT / "docs" / "pipeline_builder_schema.json"
PROFILES_INDEX = REPO_ROOT / "src" / "retdec-decompiler" / "profiles" / "index.json"

PASS_NAME_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9._-]*$")
PROFILE_NAME_RE = re.compile(r"^[a-z][a-z0-9-]*$")
VERSION_RE = re.compile(r"^\d+\.\d+(\.\d+)?$")


def _fail(errors: List[str], msg: str) -> None:
    errors.append(msg)


def _validate_pass_name(errors: List[str], value: Any, path: str) -> None:
    if not isinstance(value, str) or not value:
        _fail(errors, f"{path}: pass name must be a non-empty string")
        return
    if not PASS_NAME_RE.match(value):
        _fail(errors, f"{path}: invalid pass name {value!r}")


def _validate_pass_array(errors: List[str], data: Any, path: str = "$") -> None:
    if not isinstance(data, list):
        _fail(errors, f"{path}: expected JSON array of pass names")
        return
    if len(data) < 1:
        _fail(errors, f"{path}: pass array must contain at least one pass")
    for i, item in enumerate(data):
        _validate_pass_name(errors, item, f"{path}[{i}]")


def _validate_pipeline_document(errors: List[str], data: Any, path: str = "$") -> None:
    if not isinstance(data, dict):
        _fail(errors, f"{path}: expected pipeline document object")
        return

    allowed = {"name", "description", "version", "passes", "backend", "tags"}
    extra = set(data.keys()) - allowed
    if extra:
        _fail(errors, f"{path}: unknown keys: {sorted(extra)}")

    name = data.get("name")
    if not isinstance(name, str) or not name:
        _fail(errors, f"{path}.name: required non-empty string")
    elif not PROFILE_NAME_RE.match(name):
        _fail(errors, f"{path}.name: must match ^[a-z][a-z0-9-]*$ (got {name!r})")

    desc = data.get("description")
    if desc is not None:
        if not isinstance(desc, str):
            _fail(errors, f"{path}.description: must be a string")
        elif len(desc) > 512:
            _fail(errors, f"{path}.description: exceeds 512 characters")

    version = data.get("version")
    if version is not None:
        if not isinstance(version, str) or not VERSION_RE.match(version):
            _fail(errors, f"{path}.version: expected semver like 1.0 or 1.0.0")

    if "passes" not in data:
        _fail(errors, f"{path}.passes: required")
    else:
        _validate_pass_array(errors, data["passes"], f"{path}.passes")

    backend = data.get("backend")
    if backend is not None:
        if not isinstance(backend, dict):
            _fail(errors, f"{path}.backend: must be an object")
        else:
            if "noOpts" in backend and not isinstance(backend["noOpts"], bool):
                _fail(errors, f"{path}.backend.noOpts: must be boolean")
            for key, allowed_vals in (
                ("callInfoObtainer", {"optim", "pessim"}),
                ("varRenamer", {"address", "hungarian", "readable", "simple", "unified"}),
            ):
                if key in backend and backend[key] not in allowed_vals:
                    _fail(errors, f"{path}.backend.{key}: invalid value {backend[key]!r}")

    tags = data.get("tags")
    if tags is not None:
        if not isinstance(tags, list):
            _fail(errors, f"{path}.tags: must be an array")
        else:
            seen = set()
            for i, tag in enumerate(tags):
                if not isinstance(tag, str) or not tag:
                    _fail(errors, f"{path}.tags[{i}]: must be non-empty string")
                elif tag in seen:
                    _fail(errors, f"{path}.tags: duplicate tag {tag!r}")
                else:
                    seen.add(tag)


def validate_data(data: Any) -> Tuple[bool, List[str]]:
    errors: List[str] = []
    if isinstance(data, list):
        _validate_pass_array(errors, data)
    elif isinstance(data, dict):
        _validate_pipeline_document(errors, data)
    else:
        _fail(errors, "$: root must be a pass array or pipeline document object")
    return (len(errors) == 0, errors)


def validate_file(path: Path) -> Tuple[bool, List[str]]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        return False, [f"{path}: cannot read file: {exc}"]

    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        return False, [f"{path}: JSON parse error: {exc}"]

    ok, errors = validate_data(data)
    if not ok:
        errors = [f"{path}: {e}" if not str(e).startswith(str(path)) else e for e in errors]
    return ok, errors


def iter_profile_files() -> Iterable[Path]:
    if not PROFILES_INDEX.is_file():
        return
    index = json.loads(PROFILES_INDEX.read_text(encoding="utf-8"))
    base = PROFILES_INDEX.parent
    for entry in index.get("profiles", []):
        rel = entry.get("file")
        if not rel:
            continue
        yield (base / rel).resolve()


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate RetDec pipeline JSON files.")
    parser.add_argument("files", nargs="*", help="Pipeline JSON files to validate")
    parser.add_argument(
        "--all-profiles",
        action="store_true",
        help="Validate every profile listed in src/retdec-decompiler/profiles/index.json",
    )
    args = parser.parse_args(argv)

    if not SCHEMA_PATH.is_file():
        print(f"Error: schema missing: {SCHEMA_PATH}", file=sys.stderr)
        return 1

    paths: List[Path] = [Path(p).resolve() for p in args.files]
    if args.all_profiles:
        paths.extend(iter_profile_files())

    if not paths:
        parser.print_help()
        return 1

    seen = set()
    unique_paths = []
    for p in paths:
        if p not in seen:
            seen.add(p)
            unique_paths.append(p)

    failed = False
    for path in unique_paths:
        ok, errors = validate_file(path)
        if ok:
            print(f"OK: {path}")
        else:
            failed = True
            for err in errors:
                print(f"FAIL: {err}", file=sys.stderr)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
