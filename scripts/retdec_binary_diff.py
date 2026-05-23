#!/usr/bin/env python3
"""Cross-binary diff for RetDec security / research workflows.

Compare two PE/ELF binaries using decompile config sidecars when present,
or retdec-fileinfo JSON otherwise.  Emits structured JSON with function and
string deltas; uses identifier Jaccard similarity for paired .c bodies.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from retdec_testbench_metrics import identifiers, jaccard  # noqa: E402

_JACCARD_UNCHANGED = 0.99


@dataclass
class FunctionRecord:
    address: int
    name: str
    start_line: Optional[int] = None
    end_line: Optional[int] = None

    def as_dict(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "address": hex(self.address),
            "name": self.name,
        }
        if self.start_line is not None:
            out["startLine"] = self.start_line
        if self.end_line is not None:
            out["endLine"] = self.end_line
        return out


@dataclass
class BinarySnapshot:
    path: str
    source: str  # "config" | "fileinfo" | "empty"
    functions: Dict[int, FunctionRecord] = field(default_factory=dict)
    strings: Set[str] = field(default_factory=set)
    c_path: Optional[str] = None
    config_path: Optional[str] = None
    fileinfo_path: Optional[str] = None


def parse_addr(value: Any) -> Optional[int]:
    if value is None:
        return None
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        return None
    try:
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(text, 10)
    except ValueError:
        return None


def config_path_for_c(c_path: Path) -> Path:
    """Match RetDec GUI artifact_loader: foo.c -> foo.config.json."""
    name = str(c_path)
    if name.lower().endswith(".c"):
        name = name[:-2]
    return Path(name + ".config.json")


def discover_decompile_artifacts(binary_path: Path) -> Tuple[Optional[Path], Optional[Path]]:
    """Return (.c, .config.json) paths when a prior decompile exists."""
    binary_path = binary_path.resolve()
    stem = binary_path.with_suffix("")
    candidates_c = [
        binary_path.parent / f"{binary_path.stem}.gui-decompiled.c",
        binary_path.parent / f"{binary_path.stem}.c",
        stem.with_suffix(".gui-decompiled.c"),
        stem.with_suffix(".c"),
    ]
    for c_path in candidates_c:
        if not c_path.is_file():
            continue
        config_path = config_path_for_c(c_path)
        if config_path.is_file():
            return c_path, config_path
    return None, None


def parse_functions_from_config(config: Dict[str, Any]) -> Dict[int, FunctionRecord]:
    out: Dict[int, FunctionRecord] = {}
    for item in config.get("functions") or []:
        if not isinstance(item, dict):
            continue
        addr = parse_addr(item.get("startAddr"))
        if addr is None:
            continue
        name = str(item.get("name") or item.get("demangledName") or f"sub_{addr:x}")
        start_line = item.get("startLine")
        end_line = item.get("endLine")
        try:
            sl = int(start_line) if start_line not in (None, "") else None
        except (TypeError, ValueError):
            sl = None
        try:
            el = int(end_line) if end_line not in (None, "") else None
        except (TypeError, ValueError):
            el = None
        out[addr] = FunctionRecord(addr, name, sl, el)
    return out


def parse_strings_from_config(config: Dict[str, Any]) -> Set[str]:
    strings: Set[str] = set()
    for item in config.get("globals") or []:
        if not isinstance(item, dict):
            continue
        for key in ("realName", "name"):
            val = item.get(key)
            if isinstance(val, str) and val:
                strings.add(val)
                break
    return strings


def _walk_fileinfo_symbols(node: Any, out: Dict[int, FunctionRecord]) -> None:
    if isinstance(node, dict):
        subtitle = node.get("symbols")
        if isinstance(subtitle, list):
            for sym in subtitle:
                if not isinstance(sym, dict):
                    continue
                sym_type = str(sym.get("type", "")).upper()
                if sym_type and sym_type not in ("FUNC", "FUNCTION", "STT_FUNC"):
                    continue
                addr = parse_addr(sym.get("address") or sym.get("value"))
                if addr is None:
                    continue
                name = str(sym.get("name") or f"sym_{addr:x}")
                out.setdefault(addr, FunctionRecord(addr, name))
        for value in node.values():
            _walk_fileinfo_symbols(value, out)
    elif isinstance(node, list):
        for item in node:
            _walk_fileinfo_symbols(item, out)


def parse_strings_from_fileinfo(data: Dict[str, Any]) -> Set[str]:
    strings: Set[str] = set()

    def collect(node: Any) -> None:
        if isinstance(node, dict):
            content = node.get("content")
            if isinstance(content, str) and content:
                strings.add(content)
            for value in node.values():
                collect(value)
        elif isinstance(node, list):
            for item in node:
                collect(item)

    strings_block = data.get("strings")
    if isinstance(strings_block, dict):
        collect(strings_block.get("strings"))
    collect(data)
    return strings


def parse_functions_from_fileinfo(data: Dict[str, Any]) -> Dict[int, FunctionRecord]:
    out: Dict[int, FunctionRecord] = {}
    _walk_fileinfo_symbols(data.get("symbolTables"), out)
    if not out:
        _walk_fileinfo_symbols(data, out)
    return out


def run_fileinfo_json(binary_path: Path, fileinfo_exe: Optional[Path]) -> Dict[str, Any]:
    exe = fileinfo_exe
    if exe is None:
        for name in ("retdec-fileinfo", "fileinfo"):
            for suffix in ("", ".exe"):
                candidate = SCRIPT_DIR / f"{name}{suffix}"
                if candidate.is_file():
                    exe = candidate
                    break
            if exe:
                break
    if exe is None or not exe.is_file():
        return {}
    try:
        proc = subprocess.run(
            [str(exe), "--json", str(binary_path)],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return {}
    if proc.returncode != 0 or not proc.stdout.strip():
        return {}
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {}


def load_binary_snapshot(
    binary_path: Path,
    *,
    fileinfo_exe: Optional[Path] = None,
    fileinfo_json_path: Optional[Path] = None,
) -> BinarySnapshot:
    binary_path = binary_path.resolve()
    snap = BinarySnapshot(path=str(binary_path), source="empty")

    c_path, config_path = discover_decompile_artifacts(binary_path)
    if config_path and config_path.is_file():
        snap.config_path = str(config_path)
        if c_path and c_path.is_file():
            snap.c_path = str(c_path)
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            config = {}
        else:
            snap.functions = parse_functions_from_config(config)
            snap.strings = parse_strings_from_config(config)
            snap.source = "config"

    fi_data: Dict[str, Any] = {}
    if fileinfo_json_path and fileinfo_json_path.is_file():
        snap.fileinfo_path = str(fileinfo_json_path)
        try:
            fi_data = json.loads(fileinfo_json_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            fi_data = {}
    elif not snap.functions:
        fi_data = run_fileinfo_json(binary_path, fileinfo_exe)
        if fi_data:
            snap.source = "fileinfo"

    if fi_data:
        if not snap.functions:
            snap.functions = parse_functions_from_fileinfo(fi_data)
        if not snap.strings:
            snap.strings = parse_strings_from_fileinfo(fi_data)

    return snap


def extract_function_source(c_text: str, fn: FunctionRecord) -> str:
    if fn.start_line is None or fn.end_line is None:
        return ""
    lines = c_text.splitlines()
    start = max(1, fn.start_line)
    end = min(len(lines), fn.end_line)
    if start > end or start > len(lines):
        return ""
    return "\n".join(lines[start - 1 : end])


def diff_snapshots(left: BinarySnapshot, right: BinarySnapshot) -> Dict[str, Any]:
    left_addrs = set(left.functions)
    right_addrs = set(right.functions)

    added = [right.functions[a].as_dict() for a in sorted(right_addrs - left_addrs)]
    removed = [left.functions[a].as_dict() for a in sorted(left_addrs - right_addrs)]

    changed: List[Dict[str, Any]] = []
    left_c = Path(left.c_path).read_text(encoding="utf-8", errors="replace") if left.c_path else ""
    right_c = (
        Path(right.c_path).read_text(encoding="utf-8", errors="replace") if right.c_path else ""
    )

    for addr in sorted(left_addrs & right_addrs):
        lf = left.functions[addr]
        rf = right.functions[addr]
        entry: Dict[str, Any] = {
            "address": hex(addr),
            "left_name": lf.name,
            "right_name": rf.name,
        }
        name_changed = lf.name != rf.name
        body_score: Optional[float] = None
        if left_c and right_c and lf.start_line and lf.end_line and rf.start_line and rf.end_line:
            l_body = extract_function_source(left_c, lf)
            r_body = extract_function_source(right_c, rf)
            if l_body and r_body:
                body_score = round(jaccard(identifiers(l_body), identifiers(r_body)), 4)
                entry["jaccard"] = body_score
        if name_changed or (body_score is not None and body_score < _JACCARD_UNCHANGED):
            if name_changed:
                entry["name_changed"] = True
            changed.append(entry)

    string_diff_count = len(left.strings.symmetric_difference(right.strings))

    return {
        "left": {
            "path": left.path,
            "source": left.source,
            "function_count": len(left.functions),
            "string_count": len(left.strings),
            "c_path": left.c_path,
            "config_path": left.config_path,
        },
        "right": {
            "path": right.path,
            "source": right.source,
            "function_count": len(right.functions),
            "string_count": len(right.strings),
            "c_path": right.c_path,
            "config_path": right.config_path,
        },
        "added_functions": added,
        "removed_functions": removed,
        "changed_functions": changed,
        "string_diff_count": string_diff_count,
    }


def diff_binaries(
    left_path: Path,
    right_path: Path,
    *,
    fileinfo_exe: Optional[Path] = None,
    left_fileinfo: Optional[Path] = None,
    right_fileinfo: Optional[Path] = None,
) -> Dict[str, Any]:
    left = load_binary_snapshot(left_path, fileinfo_exe=fileinfo_exe, fileinfo_json_path=left_fileinfo)
    right = load_binary_snapshot(
        right_path, fileinfo_exe=fileinfo_exe, fileinfo_json_path=right_fileinfo
    )
    return diff_snapshots(left, right)


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare two binaries (PE/ELF) via decompile config or fileinfo.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("left", type=Path, help="First binary path")
    parser.add_argument("right", type=Path, help="Second binary path")
    parser.add_argument(
        "--fileinfo",
        type=Path,
        default=None,
        help="retdec-fileinfo executable (auto-detected beside script if omitted)",
    )
    parser.add_argument(
        "--left-fileinfo-json",
        type=Path,
        default=None,
        help="Precomputed fileinfo JSON for left binary",
    )
    parser.add_argument(
        "--right-fileinfo-json",
        type=Path,
        default=None,
        help="Precomputed fileinfo JSON for right binary",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Write JSON report to this file (default: stdout)",
    )
    return parser.parse_args(list(argv) if argv is not None else None)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    for label, path in (("left", args.left), ("right", args.right)):
        if not path.is_file():
            print(f"Error: {label} binary not found: {path}", file=sys.stderr)
            return 1

    report = diff_binaries(
        args.left,
        args.right,
        fileinfo_exe=args.fileinfo,
        left_fileinfo=args.left_fileinfo_json,
        right_fileinfo=args.right_fileinfo_json,
    )
    text = json.dumps(report, indent=2, sort_keys=False)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
