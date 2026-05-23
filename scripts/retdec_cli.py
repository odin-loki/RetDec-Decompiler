#!/usr/bin/env python3
"""
retdec_cli.py — unified RetDec command-line helper.

Subcommands:
  batch        Decompile inputs from a YAML/JSON manifest (sequential, per-file exit codes)
  diff         Compare two binaries (config/fileinfo) or two .c files (Jaccard)
  export-intel Export STIX-like threat intel JSON bundle
  emit-json    Decompile and emit one JSON report (config + strings + functions)
  watch        Poll a directory and re-decompile changed binaries
  yara-bridge  On YARA match, print suggested retdec-decompiler commands
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from retdec_testbench_metrics import (  # noqa: E402
    count_functions_c_like,
    function_names,
    identifiers,
    jaccard,
)

from retdec_binary_diff import diff_binaries  # noqa: E402


def _load_manifest(path: Path) -> List[Dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() in {".yaml", ".yml"}:
        try:
            import yaml  # type: ignore
        except ImportError as exc:
            raise SystemExit(
                "YAML manifest requires PyYAML: pip install pyyaml"
            ) from exc
        data = yaml.safe_load(text)
    else:
        data = json.loads(text)

    if isinstance(data, dict) and "inputs" in data:
        items = data["inputs"]
    elif isinstance(data, list):
        items = data
    else:
        raise SystemExit(f"manifest must be a list or {{inputs: [...]}}: {path}")

    if not isinstance(items, list):
        raise SystemExit(f"manifest inputs must be a list: {path}")
    return items


def _resolve_decompiler(explicit: Optional[str]) -> Path:
    if explicit:
        p = Path(explicit).resolve()
        if not p.is_file():
            raise SystemExit(f"decompiler not found: {p}")
        return p
    for sub in (
        "install/windows/bin/retdec-decompiler.exe",
        "install/linux/bin/retdec-decompiler",
        "build-decompiler-test/bin/retdec-decompiler.exe",
        "build-decompiler-test/bin/retdec-decompiler",
    ):
        cand = REPO_ROOT / sub
        if cand.is_file():
            return cand
    raise SystemExit(
        "retdec-decompiler not found; pass --decompiler PATH"
    )


def _run_decompiler(
    decompiler: Path,
    input_path: Path,
    output_path: Path,
    extra_args: Sequence[str],
    timeout: int,
) -> int:
    cmd = [str(decompiler), "-o", str(output_path), str(input_path), *extra_args]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return proc.returncode
    except subprocess.TimeoutExpired:
        return 124


def _config_sidecar(output_c: Path) -> Path:
    stem = output_c.with_suffix("")
    return Path(str(stem) + ".config.json")


def _extract_strings_from_config(config: Dict[str, Any]) -> List[Dict[str, str]]:
    strings: List[Dict[str, str]] = []
    for g in config.get("globals") or []:
        if not isinstance(g, dict):
            continue
        type_obj = g.get("type") or {}
        llvm_ir = str(type_obj.get("llvmIr") or "")
        is_wide = bool(type_obj.get("isWideString"))
        if "i8" not in llvm_ir and not is_wide:
            continue
        value = g.get("realName") or g.get("name") or ""
        storage = g.get("storage") or {}
        strings.append(
            {
                "address": str(storage.get("value") or ""),
                "value": str(value),
                "wide": str(is_wide).lower(),
            }
        )
    return strings


def _extract_functions_from_config(config: Dict[str, Any]) -> List[Dict[str, str]]:
    out: List[Dict[str, str]] = []
    for fn in config.get("functions") or []:
        if not isinstance(fn, dict):
            continue
        out.append(
            {
                "name": str(fn.get("name") or ""),
                "startAddr": str(fn.get("startAddr") or ""),
                "endAddr": str(fn.get("endAddr") or ""),
            }
        )
    return out


def cmd_batch(args: argparse.Namespace) -> int:
    manifest = Path(args.manifest).resolve()
    decompiler = _resolve_decompiler(args.decompiler)
    items = _load_manifest(manifest)
    out_dir = Path(args.output_dir).resolve() if args.output_dir else manifest.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    results: List[Dict[str, Any]] = []
    worst = 0
    for idx, item in enumerate(items):
        if isinstance(item, str):
            inp = Path(item)
            out = out_dir / (inp.stem + ".c")
        elif isinstance(item, dict):
            inp = Path(str(item.get("path") or item.get("input") or ""))
            out_raw = item.get("output")
            out = Path(str(out_raw)) if out_raw else out_dir / (inp.stem + ".c")
        else:
            print(f"skip invalid manifest entry at index {idx}", file=sys.stderr)
            worst = max(worst, 1)
            continue

        if not inp.is_file():
            print(f"MISSING {inp}", file=sys.stderr)
            results.append({"input": str(inp), "exit_code": 2, "error": "missing"})
            worst = max(worst, 2)
            continue

        rc = _run_decompiler(
            decompiler, inp, out, args.extra_arg, args.timeout
        )
        results.append({"input": str(inp), "output": str(out), "exit_code": rc})
        status = "OK" if rc == 0 else f"FAIL({rc})"
        print(f"[{idx + 1}/{len(items)}] {status} {inp.name} -> {out}")
        if rc != 0:
            worst = max(worst, rc if rc > 0 else 1)

    if args.report:
        Path(args.report).write_text(json.dumps(results, indent=2), encoding="utf-8")
    return worst


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _diff_c_files(left: Path, right: Path) -> Dict[str, Any]:
    left_code = _read_text(left)
    right_code = _read_text(right)
    left_names = function_names(left_code)
    right_names = function_names(right_code)
    body_jaccard = round(jaccard(identifiers(left_code), identifiers(right_code)), 4)
    return {
        "mode": "c_files",
        "left": str(left),
        "right": str(right),
        "left_function_count": count_functions_c_like(left_code),
        "right_function_count": count_functions_c_like(right_code),
        "function_name_jaccard": jaccard(left_names, right_names),
        "identifier_jaccard": body_jaccard,
        "added_function_names": sorted(right_names - left_names),
        "removed_function_names": sorted(left_names - right_names),
    }


def cmd_diff(args: argparse.Namespace) -> int:
    left = Path(args.left).resolve()
    right = Path(args.right).resolve()
    if not left.is_file() or not right.is_file():
        print("both inputs must exist", file=sys.stderr)
        return 2

    if left.suffix.lower() == ".c" and right.suffix.lower() == ".c":
        report = _diff_c_files(left, right)
    else:
        fileinfo_exe = Path(args.fileinfo).resolve() if args.fileinfo else None
        report = diff_binaries(
            left,
            right,
            fileinfo_exe=fileinfo_exe,
            left_fileinfo=Path(args.left_fileinfo_json).resolve()
            if args.left_fileinfo_json
            else None,
            right_fileinfo=Path(args.right_fileinfo_json).resolve()
            if args.right_fileinfo_json
            else None,
        )
        report["mode"] = "binaries"

    text = json.dumps(report, indent=2)
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0 if "error" not in report else 1


def cmd_export_intel(args: argparse.Namespace) -> int:
    from retdec_export_intel import build_intel_bundle, resolve_inputs

    c_path = Path(args.c_path).resolve() if args.c_path else None
    config_path = Path(args.config_path).resolve() if args.config_path else None
    binary_path = Path(args.binary_path).resolve() if args.binary_path else None
    c_path, config_path, binary_path = resolve_inputs(c_path, config_path, binary_path)
    fileinfo_path = Path(args.fileinfo_path).resolve() if args.fileinfo_path else None

    if not config_path and not fileinfo_path and not c_path:
        print(
            "provide --config, --c, and/or --fileinfo (or --binary with sidecars)",
            file=sys.stderr,
        )
        return 1

    bundle = build_intel_bundle(
        c_path=c_path,
        config_path=config_path,
        fileinfo_path=fileinfo_path,
        binary_path=binary_path,
        strings_sample_limit=args.strings_limit,
    )
    out = Path(args.output).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
    print(str(out))
    return 0


def cmd_yara_bridge(args: argparse.Namespace) -> int:
    from yara_retdec_bridge import main as yara_main

    argv = [args.rule, args.directory, "--decompiler", args.decompiler]
    if args.limit:
        argv.extend(["--limit", str(args.limit)])
    return int(yara_main(argv))


def cmd_emit_json(args: argparse.Namespace) -> int:
    inp = Path(args.input).resolve()
    if not inp.is_file():
        print(f"input not found: {inp}", file=sys.stderr)
        return 2

    decompiler = _resolve_decompiler(args.decompiler)
    out_c = Path(args.output).resolve() if args.output else inp.with_suffix(".c")
    rc = _run_decompiler(decompiler, inp, out_c, args.extra_arg, args.timeout)

    cfg_path = _config_sidecar(out_c)
    config: Dict[str, Any] = {}
    if cfg_path.is_file():
        try:
            config = json.loads(cfg_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            config = {}

    report = {
        "input": str(inp),
        "output_c": str(out_c),
        "config_path": str(cfg_path) if cfg_path.is_file() else "",
        "decompile_exit_code": rc,
        "functions": _extract_functions_from_config(config),
        "strings": _extract_strings_from_config(config),
        "function_count": len(_extract_functions_from_config(config)),
        "string_count": len(_extract_strings_from_config(config)),
        "config": config if args.include_full_config else None,
    }
    if not args.include_full_config:
        report.pop("config")

    out_json = Path(args.json_output).resolve() if args.json_output else out_c.with_suffix(".report.json")
    out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(str(out_json))
    return rc if rc != 0 else 0


def _binary_mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def cmd_watch(args: argparse.Namespace) -> int:
    watch_dir = Path(args.directory).resolve()
    if not watch_dir.is_dir():
        print(f"not a directory: {watch_dir}", file=sys.stderr)
        return 2

    decompiler = _resolve_decompiler(args.decompiler)
    out_dir = Path(args.output_dir).resolve() if args.output_dir else watch_dir / "_decompiled"
    out_dir.mkdir(parents=True, exist_ok=True)

    seen: Dict[str, float] = {}
    exts = {e.lower() for e in args.extension}

    print(f"watching {watch_dir} (poll {args.interval}s), output -> {out_dir}")
    try:
        while True:
            for entry in watch_dir.iterdir():
                if not entry.is_file():
                    continue
                if entry.suffix.lower() not in exts:
                    continue
                mtime = _binary_mtime(entry)
                key = str(entry)
                if seen.get(key) == mtime:
                    continue
                out_c = out_dir / (entry.stem + ".c")
                rc = _run_decompiler(
                    decompiler, entry, out_c, args.extra_arg, args.timeout
                )
                seen[key] = mtime
                print(
                    f"{time.strftime('%H:%M:%S')} {entry.name} -> {out_c.name} exit={rc}"
                )
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nwatch stopped")
        return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--decompiler",
        help="Path to retdec-decompiler (auto-detected from install/ or build/)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_batch = sub.add_parser("batch", help="Decompile manifest entries sequentially")
    p_batch.add_argument("manifest", help="YAML or JSON manifest of inputs")
    p_batch.add_argument("--output-dir", help="Directory for .c outputs")
    p_batch.add_argument("--report", help="Write per-input results JSON here")
    p_batch.add_argument("--timeout", type=int, default=600)
    p_batch.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Extra retdec-decompiler argument (repeatable)",
    )
    p_batch.set_defaults(func=cmd_batch)

    p_diff = sub.add_parser("diff", help="Compare two .c files or two binaries")
    p_diff.add_argument("left")
    p_diff.add_argument("right")
    p_diff.add_argument("--output", "-o", help="Write diff JSON to file")
    p_diff.add_argument(
        "--fileinfo",
        help="retdec-fileinfo executable for binaries without decompile sidecars",
    )
    p_diff.add_argument("--left-fileinfo-json", help="Precomputed fileinfo JSON (left)")
    p_diff.add_argument("--right-fileinfo-json", help="Precomputed fileinfo JSON (right)")
    p_diff.set_defaults(func=cmd_diff)

    p_intel = sub.add_parser("export-intel", help="Export STIX-like threat intel bundle")
    p_intel.add_argument("--c", dest="c_path", help="Decompiled .c path")
    p_intel.add_argument("--config", dest="config_path", help=".config.json sidecar")
    p_intel.add_argument("--binary", dest="binary_path", help="Original binary (hashing)")
    p_intel.add_argument("--fileinfo", dest="fileinfo_path", help="fileinfo JSON")
    p_intel.add_argument("-o", "--output", required=True, help="Output JSON path")
    p_intel.add_argument("--strings-limit", type=int, default=32)
    p_intel.set_defaults(func=cmd_export_intel)

    p_yara = sub.add_parser("yara-bridge", help="YARA scan -> suggested decompiler commands")
    p_yara.add_argument("rule", help="YARA rule file")
    p_yara.add_argument("directory", help="Directory to scan")
    p_yara.add_argument("--decompiler", default="retdec-decompiler")
    p_yara.add_argument("--limit", type=int, default=0)
    p_yara.set_defaults(func=cmd_yara_bridge)

    p_emit = sub.add_parser(
        "emit-json", help="Decompile and aggregate config/strings/functions"
    )
    p_emit.add_argument("input", help="Binary to decompile")
    p_emit.add_argument("--output", "-o", help="Output .c path")
    p_emit.add_argument("--json-output", help="Aggregated JSON report path")
    p_emit.add_argument(
        "--include-full-config",
        action="store_true",
        help="Embed full .config.json in report",
    )
    p_emit.add_argument("--timeout", type=int, default=600)
    p_emit.add_argument("--extra-arg", action="append", default=[])
    p_emit.set_defaults(func=cmd_emit_json)

    p_watch = sub.add_parser("watch", help="Poll directory and re-decompile changes")
    p_watch.add_argument("directory", help="Directory to watch")
    p_watch.add_argument("--output-dir", help="Where to write decompiled .c files")
    p_watch.add_argument("--interval", type=float, default=5.0, help="Poll seconds")
    p_watch.add_argument(
        "--extension",
        action="append",
        default=[".exe", ".bin", ".elf", ".dll", ""],
        help="File extension to watch (repeatable; empty = no extension)",
    )
    p_watch.add_argument("--timeout", type=int, default=600)
    p_watch.add_argument("--extra-arg", action="append", default=[])
    p_watch.set_defaults(func=cmd_watch)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
