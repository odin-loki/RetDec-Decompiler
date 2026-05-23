#!/usr/bin/env python3
"""Export decompilation artifacts as a STIX-like threat intel JSON bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Set

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from retdec_binary_diff import (  # noqa: E402
    config_path_for_c,
    discover_decompile_artifacts,
    parse_functions_from_config,
    parse_strings_from_config,
    parse_strings_from_fileinfo,
)


EXPORT_VERSION = "1.0"
STIX_SPEC = "2.1"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sample_strings(strings: Set[str], limit: int = 32) -> List[str]:
    ordered = sorted(strings, key=lambda s: (-len(s), s.lower()))
    out: List[str] = []
    for s in ordered:
        if not s or len(s) < 4:
            continue
        out.append(s[:512])
        if len(out) >= limit:
            break
    return out


def semantic_detections_from_config(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    detections: List[Dict[str, Any]] = []
    for item in config.get("patterns") or []:
        if not isinstance(item, dict):
            continue
        detections.append(
            {
                "kind": "pattern",
                "name": item.get("name") or item.get("pattern"),
                "description": item.get("description") or "",
                "confidence": item.get("confidence"),
            }
        )
    for item in config.get("tools") or []:
        if not isinstance(item, dict):
            continue
        detections.append(
            {
                "kind": "tool",
                "name": item.get("name") or item.get("tool"),
                "version": item.get("version"),
            }
        )
    return detections


def semantic_detections_from_fileinfo(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    detections: List[Dict[str, Any]] = []

    def walk(node: Any, path: str = "") -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                lk = key.lower()
                if lk in ("detections", "matches", "rules", "yaraRules"):
                    if isinstance(value, list):
                        for entry in value:
                            if isinstance(entry, dict):
                                detections.append(
                                    {
                                        "kind": "fileinfo",
                                        "category": path or key,
                                        "name": entry.get("name") or entry.get("rule"),
                                        "description": entry.get("description") or entry.get("meta"),
                                    }
                                )
                elif lk in ("compiler", "language", "languages"):
                    if isinstance(value, (str, dict, list)):
                        detections.append({"kind": "fileinfo", "category": key, "value": value})
                else:
                    walk(value, key)
        elif isinstance(node, list):
            for item in node:
                walk(item, path)

    walk(data)
    return detections[:64]


def build_intel_bundle(
    *,
    c_path: Optional[Path] = None,
    config_path: Optional[Path] = None,
    fileinfo_path: Optional[Path] = None,
    binary_path: Optional[Path] = None,
    strings_sample_limit: int = 32,
) -> Dict[str, Any]:
    now = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")

    config: Dict[str, Any] = {}
    if config_path and config_path.is_file():
        config = json.loads(config_path.read_text(encoding="utf-8"))

    fileinfo: Dict[str, Any] = {}
    if fileinfo_path and fileinfo_path.is_file():
        fileinfo = json.loads(fileinfo_path.read_text(encoding="utf-8"))

    functions = parse_functions_from_config(config) if config else {}
    strings: Set[str] = set()
    if config:
        strings |= parse_strings_from_config(config)
    if fileinfo:
        strings |= parse_strings_from_fileinfo(fileinfo)

    hashes: Dict[str, str] = {}
    hash_source = binary_path or c_path
    if hash_source and hash_source.is_file():
        hashes["SHA-256"] = sha256_file(hash_source)

    semantic = semantic_detections_from_config(config)
    semantic.extend(semantic_detections_from_fileinfo(fileinfo))

    file_obj_id = f"file--{uuid.uuid4()}"
    indicator_id = f"indicator--{uuid.uuid4()}"

    file_obj: Dict[str, Any] = {
        "type": "file",
        "spec_version": STIX_SPEC,
        "id": file_obj_id,
        "created": now,
        "modified": now,
        "name": (binary_path or c_path or config_path or Path("unknown")).name,
        "hashes": hashes,
        "x_retdec_function_count": len(functions),
        "x_retdec_strings_sample": sample_strings(strings, strings_sample_limit),
        "x_retdec_semantic_detections": semantic,
        "x_retdec_artifacts": {
            "c": str(c_path) if c_path else None,
            "config": str(config_path) if config_path else None,
            "binary": str(binary_path) if binary_path else None,
        },
    }

    indicator: Dict[str, Any] = {
        "type": "indicator",
        "spec_version": STIX_SPEC,
        "id": indicator_id,
        "created": now,
        "modified": now,
        "name": f"RetDec decompilation summary — {file_obj['name']}",
        "pattern_type": "stix",
        "pattern": f"[file:hashes.'SHA-256' = '{hashes.get('SHA-256', '')}']",
        "valid_from": now,
        "labels": ["malware-analysis", "retdec-decompilation"],
        "x_retdec_function_count": len(functions),
        "x_retdec_string_count": len(strings),
    }

    return {
        "type": "bundle",
        "id": f"bundle--{uuid.uuid4()}",
        "spec_version": STIX_SPEC,
        "x_retdec_export_version": EXPORT_VERSION,
        "x_retdec_generated": now,
        "objects": [file_obj, indicator],
    }


def resolve_inputs(
    c_path: Optional[Path],
    config_path: Optional[Path],
    binary_path: Optional[Path],
) -> tuple[Optional[Path], Optional[Path], Optional[Path]]:
    if c_path and c_path.is_file() and not config_path:
        candidate = config_path_for_c(c_path)
        if candidate.is_file():
            config_path = candidate
    if binary_path and binary_path.is_file() and not (c_path and config_path):
        discovered_c, discovered_cfg = discover_decompile_artifacts(binary_path)
        c_path = c_path or discovered_c
        config_path = config_path or discovered_cfg
    return c_path, config_path, binary_path


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export STIX-like threat intel JSON from RetDec artifacts.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--c", type=Path, help="Decompiled .c path")
    parser.add_argument("--config", type=Path, help="Decompiler .config.json sidecar")
    parser.add_argument("--binary", type=Path, help="Original binary (for hashing)")
    parser.add_argument("--fileinfo", type=Path, help="Optional retdec-fileinfo JSON")
    parser.add_argument(
        "--strings-limit",
        type=int,
        default=32,
        help="Maximum strings in the sample list",
    )
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output JSON path")
    return parser.parse_args(list(argv) if argv is not None else None)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    c_path, config_path, binary_path = resolve_inputs(args.c, args.config, args.binary)

    if not config_path and not args.fileinfo and not c_path:
        print(
            "Error: provide --config, --c, and/or --fileinfo (or --binary with sidecars)",
            file=sys.stderr,
        )
        return 1

    bundle = build_intel_bundle(
        c_path=c_path,
        config_path=config_path,
        fileinfo_path=args.fileinfo,
        binary_path=binary_path,
        strings_sample_limit=args.strings_limit,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
