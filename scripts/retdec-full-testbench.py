#!/usr/bin/env python3

"""Full, batch-based RetDec testbench runner."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import importlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional

from retdec_testbench_metrics import count_functions_c_like, identifiers, jaccard

utils = importlib.import_module("retdec-utils")
utils.check_python_version()
CmdRunner = utils.CmdRunner

TIMEOUT_RC = utils.TIMEOUT_RC
BAD_ALLOC_RC = utils.BAD_ALLOC_RC

PROFILES = {
    "quick": {"max_inputs": 20, "batch_size": 4, "timeout": 180, "retries": 1},
    "balanced": {"max_inputs": 120, "batch_size": 8, "timeout": 300, "retries": 2},
    "deep": {"max_inputs": 500, "batch_size": 10, "timeout": 600, "retries": 2},
}


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run full RetDec testbench in small batches: unit/component tests, "
            "CLI E2E decompilation, quality metrics, performance, and stability."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--workspace", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--build-dir", required=True, help="CMake build directory")
    parser.add_argument(
        "--retdec-bin-dir",
        default="",
        help="Directory containing retdec binaries (retdec-decompiler, etc.)",
    )
    parser.add_argument("--results-dir", default="", help="Where results are written")
    parser.add_argument("--profile", choices=sorted(PROFILES.keys()), default="balanced")
    parser.add_argument("--batch-size", type=int, default=0, help="Override batch size")
    parser.add_argument("--timeout", type=int, default=0, help="Per-input timeout seconds")
    parser.add_argument("--retries", type=int, default=-1, help="Retries for failed inputs")
    parser.add_argument(
        "--non-blocking",
        action="store_true",
        help="Never stop after failures; report all errors at the end",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Resume from results-dir/campaign_state.json if present",
    )
    parser.add_argument(
        "--max-inputs",
        type=int,
        default=0,
        help="Override profile max inputs",
    )
    parser.add_argument(
        "--extra-decompiler-arg",
        action="append",
        default=[],
        help="Extra argument passed to retdec-decompiler (can be repeated)",
    )
    return parser.parse_args(argv)


def now_stamp() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def is_probable_binary(path: Path) -> bool:
    if not path.is_file():
        return False
    if path.suffix.lower() in {".a", ".so", ".dll", ".dylib", ".o", ".obj", ".exe", ".bin", ".elf", ".lib"}:
        return True
    # If extension is unknown (or absent), verify binary magic.
    try:
        with path.open("rb") as f:
            head = f.read(8)
    except OSError:
        return False

    if head.startswith(b"MZ"):
        return True
    if head.startswith(b"\x7fELF"):
        return True
    if head.startswith(b"!<arch>\n"):
        return True
    if head[:4] in {
        b"\xfe\xed\xfa\xce",  # Mach-O 32
        b"\xfe\xed\xfa\xcf",  # Mach-O 64
        b"\xce\xfa\xed\xfe",  # Mach-O 32 (rev)
        b"\xcf\xfa\xed\xfe",  # Mach-O 64 (rev)
        b"\xca\xfe\xba\xbe",  # FAT
        b"\xbe\xba\xfe\xca",  # FAT (rev)
    }:
        return True

    return False


def discover_retdec_bin_dir(build_dir: Path, explicit: str) -> Path:
    if explicit:
        p = Path(explicit).expanduser().resolve()
        if p.is_dir():
            return p
    candidates = [
        build_dir / "install" / "bin",
        build_dir / "bin",
        build_dir,
    ]
    for c in candidates:
        for name in ("retdec-decompiler", "retdec-decompiler.exe", "retdec-decompiler.py"):
            if (c / name).is_file():
                return c
    for name in ("retdec-decompiler", "retdec-decompiler.exe", "retdec-decompiler.py"):
        hits = list(build_dir.rglob(name))
        file_hits = [h for h in hits if h.is_file()]
        if file_hits:
            return file_hits[0].parent
    raise RuntimeError(
        "Unable to locate retdec binary directory; pass --retdec-bin-dir explicitly."
    )


def find_tool(bin_dir: Path, names: Iterable[str]) -> Optional[Path]:
    for name in names:
        p = bin_dir / name
        if p.exists() and p.is_file():
            return p
    return None


def discover_archive_decompiler(
    workspace: Path, build_dir: Path, bin_dir: Path
) -> Optional[Path]:
    """Locate archive decompiler helper with robust fallbacks."""
    direct = find_tool(bin_dir, ("retdec-archive-decompiler.py", "retdec-archive-decompiler"))
    if direct is not None:
        return direct

    candidates = [
        workspace / "scripts" / "retdec-archive-decompiler.py",
        build_dir / "scripts" / "retdec-archive-decompiler.py",
        build_dir / "install" / "bin" / "retdec-archive-decompiler.py",
    ]
    for c in candidates:
        if c.exists() and c.is_file():
            return c
    return None


def discover_tool_in_build(build_dir: Path, names: Iterable[str]) -> Optional[Path]:
    """Find first matching executable/file anywhere under build tree."""
    for name in names:
        hits = [h for h in build_dir.rglob(name) if h.is_file()]
        if hits:
            return hits[0]
    return None


def load_state(state_path: Path) -> Dict:
    if not state_path.exists():
        return {"completed_inputs": [], "phases": {}, "results": []}
    with state_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def save_state(state_path: Path, state: Dict) -> None:
    with state_path.open("w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, sort_keys=True)


def chunked(items: List[Dict], n: int) -> Iterable[List[Dict]]:
    for i in range(0, len(items), n):
        yield items[i : i + n]


def to_tool_path_arg(tool: Path, p: Path) -> str:
    """
    Convert POSIX paths to Windows when invoking a Windows .exe from WSL.
    """
    s = str(p)
    if (
        tool.suffix.lower() == ".exe"
        and sys.platform.startswith("linux")
        and s.startswith("/mnt/")
    ):
        try:
            return (
                subprocess.check_output(["wslpath", "-w", s], text=True)
                .strip()
            )
        except Exception:
            return s
    return s


def classify_run(rc: int, timeouted: bool, output: str) -> str:
    if timeouted or rc == TIMEOUT_RC:
        return "timeout"
    if rc == BAD_ALLOC_RC or "std::bad_alloc" in (output or ""):
        return "oom"
    if rc == 0:
        return "ok"
    return "fail"


def parse_recompile_score(output: str) -> Optional[int]:
    m = re.search(r"Score:\s+(\d+)%\s+recompilable", output or "")
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def discover_source_pairs(workspace: Path) -> Dict[str, Path]:
    src_map: Dict[str, Path] = {}
    for root in (workspace / "tests", workspace / "src"):
        if not root.exists():
            continue
        for ext in ("*.c", "*.cc", "*.cpp"):
            for p in root.rglob(ext):
                src_map.setdefault(p.stem.lower(), p)
    return src_map


def discover_inputs(build_dir: Path, workspace: Path, max_inputs: int) -> List[Dict]:
    src_map = discover_source_pairs(workspace)
    seen: set = set()
    items: List[Dict] = []
    win_smoke_mode = "build-win" in str(build_dir).lower()

    if win_smoke_mode:
        roots = [
            build_dir / "win-layout" / "bin",
            build_dir / "win-runtime",
            build_dir / "deps" / "install" / "llvm" / "bin",
            build_dir / "deps" / "yara" / "yara" / "src" / "yara",
        ]
    else:
        roots = [build_dir, workspace / "tests", workspace / "deps"]
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if not is_probable_binary(p):
                continue
            if win_smoke_mode and p.suffix.lower() != ".exe":
                continue
            # Keep archives and regular binaries, skip known build internals.
            lower = str(p).lower()
            if any(
                part in lower
                for part in (
                    "cmakefiles",
                    ".git",
                    "llvm/test",
                    "unittest",
                    "gtest",
                    "gmock",
                )
            ):
                continue
            key = str(p.resolve())
            if key in seen:
                continue
            seen.add(key)
            items.append(
                {
                    "name": p.name,
                    "path": key,
                    "kind": "archive" if p.suffix.lower() == ".a" else "binary",
                    "source": str(src_map[p.stem.lower()]) if p.stem.lower() in src_map else "",
                }
            )
            if len(items) >= max_inputs:
                return items
    return items


def run_unit_and_component_tests(build_dir: Path, bin_dir: Path, logs_dir: Path) -> Dict:
    result = {"unit_runner_rc": None, "unit_bins_total": 0, "unit_bins_failed": 0, "component_rc": None}

    runner = find_tool(bin_dir, ("retdec-tests-runner.py", "retdec-tests-runner"))
    if runner:
        out, rc, _ = CmdRunner.run_cmd(
            [sys.executable, str(runner), "--verbose"],
            buffer_output=True,
            print_run_msg=True,
        )
        (logs_dir / "unit_runner.log").write_text(out or "", encoding="utf-8")
        result["unit_runner_rc"] = rc
    else:
        tests = [p for p in build_dir.rglob("retdec-tests-*") if p.is_file()]
        result["unit_bins_total"] = len(tests)
        failed = 0
        for t in sorted(tests):
            out, rc, _ = CmdRunner.run_cmd([str(t), "--gtest_color=yes"], buffer_output=True)
            (logs_dir / f"{t.name}.log").write_text(out or "", encoding="utf-8")
            if rc != 0:
                failed += 1
        result["unit_bins_failed"] = failed

    component_tests_dir = Path(__file__).resolve().parent / "type_extractor" / "tests"
    if component_tests_dir.exists():
        out, rc, _ = CmdRunner.run_cmd(
            [
                sys.executable,
                "-m",
                "unittest",
                "discover",
                "-s",
                str(component_tests_dir),
                "-p",
                "*_tests.py",
            ],
            buffer_output=True,
            print_run_msg=True,
        )
        (logs_dir / "component_tests.log").write_text(out or "", encoding="utf-8")
        result["component_rc"] = rc
    return result


def evaluate_quality(
    item: Dict,
    out_c: Path,
    check_recompile: Path,
    decompiler: Path,
    timeout: int,
) -> Dict:
    quality = {
        "decompiled_fn_count": None,
        "source_fn_count": None,
        "fn_ratio": None,
        "identifier_jaccard": None,
        "recompile_score": None,
    }

    if out_c.exists():
        code = out_c.read_text(encoding="utf-8", errors="replace")
        quality["decompiled_fn_count"] = count_functions_c_like(code)
        if item.get("source"):
            src_path = Path(item["source"])
            if src_path.exists():
                src_code = src_path.read_text(encoding="utf-8", errors="replace")
                src_cnt = count_functions_c_like(src_code)
                dec_cnt = quality["decompiled_fn_count"] or 0
                quality["source_fn_count"] = src_cnt
                quality["fn_ratio"] = round(dec_cnt / max(src_cnt, 1), 3)
                quality["identifier_jaccard"] = round(
                    jaccard(identifiers(src_code), identifiers(code)), 3
                )

    # Recompilation metric path (best-effort and non-blocking).
    if check_recompile.exists() and utils.tool_exists("bash") and utils.tool_exists("gcc"):
        out, _, _ = CmdRunner.run_cmd(
            ["bash", str(check_recompile), item["path"], str(decompiler)],
            timeout=timeout,
            buffer_output=True,
            print_run_msg=True,
        )
        quality["recompile_score"] = parse_recompile_score(out or "")
    return quality


def run_cli_batch(
    batch: List[Dict],
    decompiler: Path,
    archive_decompiler_script: Optional[Path],
    ar_extractor_tool: Optional[Path],
    outputs_dir: Path,
    logs_dir: Path,
    timeout: int,
    retries: int,
    extra_args: List[str],
    check_recompile: Path,
) -> List[Dict]:
    batch_results: List[Dict] = []
    for item in batch:
        item_name = item["name"]
        src_path = Path(item["path"])
        out_c = outputs_dir / f"{item_name}.c"
        log_file = logs_dir / f"{item_name}.log"
        entry = {
            "name": item_name,
            "path": item["path"],
            "kind": item["kind"],
            "source": item.get("source", ""),
            "status": "pending",
            "attempts": [],
            "elapsed_s": None,
            "memory_mb": None,
        }

        for attempt in range(retries + 1):
            if item["kind"] == "archive" and ar_extractor_tool is not None:
                cnt_out, cnt_rc, _ = CmdRunner.run_cmd(
                    [
                        str(ar_extractor_tool),
                        to_tool_path_arg(ar_extractor_tool, src_path),
                        "--object-count",
                    ],
                    buffer_output=True,
                    print_run_msg=False,
                )
                try:
                    raw = (cnt_out or "").strip()
                    last_line = raw.splitlines()[-1] if raw else ""
                    object_count = int(last_line) if cnt_rc == 0 else -1
                except ValueError:
                    object_count = -1

                if object_count <= 0:
                    memory, elapsed, output, rc, timeouted = (0, 1, cnt_out or "", 1, False)
                else:
                    all_out: List[str] = []
                    max_mem = 0
                    sum_elapsed = 0
                    timeouted = False
                    rc = 0
                    for idx in range(object_count):
                        indexed_out = outputs_dir / f"{item_name}.file_{idx + 1}.c"
                        cmd = [
                            str(decompiler),
                            f"--ar-index={idx}",
                            "-o",
                            to_tool_path_arg(decompiler, indexed_out),
                            to_tool_path_arg(decompiler, src_path),
                        ] + extra_args
                        mem_i, elapsed_i, out_i, rc_i = CmdRunner.run_measured_cmd(
                            cmd, timeout=timeout, print_run_msg=True
                        )
                        all_out.append(f"[archive-object {idx + 1}/{object_count}]")
                        all_out.append(out_i or "")
                        max_mem = max(max_mem, mem_i)
                        sum_elapsed += elapsed_i
                        if rc_i == TIMEOUT_RC:
                            timeouted = True
                            rc = rc_i
                            break
                        if rc_i != 0:
                            rc = rc_i
                    memory = max_mem
                    elapsed = max(sum_elapsed, 1)
                    output = "\n".join(all_out).strip()
            elif item["kind"] == "archive" and archive_decompiler_script is not None:
                cmd = [sys.executable, str(archive_decompiler_script), str(src_path)]
                memory, elapsed, output, rc = CmdRunner.run_measured_cmd(
                    cmd, timeout=timeout, print_run_msg=True
                )
                timeouted = rc == TIMEOUT_RC
            else:
                cmd = [
                    str(decompiler),
                    to_tool_path_arg(decompiler, src_path),
                    "-o",
                    to_tool_path_arg(decompiler, out_c),
                ] + extra_args
                memory, elapsed, output, rc = CmdRunner.run_measured_cmd(cmd, timeout=timeout, print_run_msg=True)
                timeouted = rc == TIMEOUT_RC

            status = classify_run(rc, timeouted, output or "")
            entry["attempts"].append({"attempt": attempt + 1, "rc": rc, "status": status})
            entry["elapsed_s"] = elapsed
            entry["memory_mb"] = memory
            if status == "ok":
                break

        final_status = entry["attempts"][-1]["status"]
        if len(entry["attempts"]) > 1 and final_status == "ok":
            entry["status"] = "recovered"
        else:
            entry["status"] = final_status

        log_file.write_text(output or "", encoding="utf-8")
        quality = evaluate_quality(item, out_c, check_recompile, decompiler, timeout)
        entry.update(quality)
        batch_results.append(entry)
    return batch_results


def write_outputs(results_dir: Path, state: Dict) -> None:
    results_json = results_dir / "results.json"
    results_csv = results_dir / "metrics.csv"
    summary_md = results_dir / "summary.md"

    with results_json.open("w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, sort_keys=True)

    rows = state.get("results", [])
    fieldnames = [
        "name",
        "path",
        "kind",
        "status",
        "elapsed_s",
        "memory_mb",
        "decompiled_fn_count",
        "source_fn_count",
        "fn_ratio",
        "identifier_jaccard",
        "recompile_score",
    ]
    with results_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in rows:
            writer.writerow({k: r.get(k) for k in fieldnames})

    total = len(rows)
    ok = len([r for r in rows if r.get("status") in ("ok", "recovered")])
    failed = total - ok
    timeouts = len([r for r in rows if r.get("status") == "timeout"])
    recovered = len([r for r in rows if r.get("status") == "recovered"])
    avg_elapsed = round(
        sum(float(r.get("elapsed_s") or 0) for r in rows) / max(total, 1), 2
    )
    avg_mem = round(
        sum(float(r.get("memory_mb") or 0) for r in rows) / max(total, 1), 2
    )
    recompiles = [r.get("recompile_score") for r in rows if r.get("recompile_score") is not None]
    avg_recompile = round(sum(recompiles) / len(recompiles), 2) if recompiles else None

    summary = [
        "# RetDec Full Testbench Report",
        "",
        "## Campaign",
        f"- Generated: {dt.datetime.now().isoformat()}",
        f"- Inputs processed: {total}",
        f"- Successful (including recovered): {ok}",
        f"- Failed: {failed}",
        f"- Timeouts: {timeouts}",
        f"- Recovered after retry: {recovered}",
        "",
        "## Performance",
        f"- Average decompile time (s): {avg_elapsed}",
        f"- Average peak memory (MB): {avg_mem}",
        "",
        "## Quality",
        f"- Average recompilability (%): {avg_recompile if avg_recompile is not None else 'n/a'}",
        "",
        "## Artifacts",
        "- `results.json`: full structured output",
        "- `metrics.csv`: flat metrics table",
        "- `logs/`: per-input and phase logs",
        "- `outputs/`: decompiled C outputs",
    ]
    summary_md.write_text("\n".join(summary) + "\n", encoding="utf-8")


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    profile = PROFILES[args.profile]
    timeout = args.timeout if args.timeout > 0 else profile["timeout"]
    retries = args.retries if args.retries >= 0 else profile["retries"]
    max_inputs = args.max_inputs if args.max_inputs > 0 else profile["max_inputs"]
    batch_size = args.batch_size if args.batch_size > 0 else profile["batch_size"]

    workspace = Path(args.workspace).expanduser().resolve()
    build_dir = Path(args.build_dir).expanduser().resolve()
    if not build_dir.exists():
        print(f"error: build dir does not exist: {build_dir}", file=sys.stderr)
        return 2

    if args.results_dir:
        results_dir = Path(args.results_dir).expanduser().resolve()
    else:
        results_dir = workspace / "testbench-results" / now_stamp()
    logs_dir = results_dir / "logs"
    outputs_dir = results_dir / "outputs"
    ensure_dir(logs_dir)
    ensure_dir(outputs_dir)

    state_path = results_dir / "campaign_state.json"
    state = load_state(state_path) if args.resume else {"completed_inputs": [], "phases": {}, "results": []}

    bin_dir = discover_retdec_bin_dir(build_dir, args.retdec_bin_dir)
    decompiler = find_tool(bin_dir, ("retdec-decompiler", "retdec-decompiler.exe", "retdec-decompiler.py"))
    if decompiler is None:
        print(f"error: retdec-decompiler not found under {bin_dir}", file=sys.stderr)
        return 3
    archive_decompiler_script = discover_archive_decompiler(workspace, build_dir, bin_dir)
    ar_extractor_tool = (
        find_tool(bin_dir, ("retdec-ar-extractor", "retdec-ar-extractor.exe"))
        or discover_tool_in_build(build_dir, ("retdec-ar-extractor", "retdec-ar-extractor.exe"))
    )
    check_recompile = workspace / "check_recompile.sh"

    print(f"[testbench] workspace={workspace}")
    print(f"[testbench] build_dir={build_dir}")
    print(f"[testbench] bin_dir={bin_dir}")
    print(f"[testbench] results_dir={results_dir}")
    print(f"[testbench] profile={args.profile}, batch_size={batch_size}, timeout={timeout}, retries={retries}")

    # Phase 1: existing reusable test runners.
    if not state["phases"].get("inventory_reuse_done"):
        phase_result = run_unit_and_component_tests(build_dir, bin_dir, logs_dir)
        state["phases"]["inventory_reuse_done"] = True
        state["phases"]["inventory_reuse"] = phase_result
        save_state(state_path, state)

    # Phase 2-4: campaign driver + quality/perf/stability.
    inputs = discover_inputs(build_dir, workspace, max_inputs=max_inputs)
    completed = set(state.get("completed_inputs", []))
    pending = [i for i in inputs if i["path"] not in completed]
    print(f"[testbench] discovered_inputs={len(inputs)}, pending={len(pending)}")

    for idx, batch in enumerate(chunked(pending, batch_size), start=1):
        print(f"[testbench] batch {idx}: {len(batch)} inputs")
        batch_results = run_cli_batch(
            batch=batch,
            decompiler=decompiler,
            archive_decompiler_script=archive_decompiler_script,
            ar_extractor_tool=ar_extractor_tool,
            outputs_dir=outputs_dir,
            logs_dir=logs_dir,
            timeout=timeout,
            retries=retries,
            extra_args=args.extra_decompiler_arg,
            check_recompile=check_recompile,
        )
        state["results"].extend(batch_results)
        state["completed_inputs"].extend([i["path"] for i in batch])
        save_state(state_path, state)

        # Non-blocking by default, but keep option for strict mode.
        if not args.non_blocking:
            critical = [r for r in batch_results if r["status"] in ("oom", "timeout", "fail")]
            if critical:
                print("[testbench] stopping early due to strict mode failure")
                break

    state["phases"]["campaign_done"] = True
    save_state(state_path, state)
    write_outputs(results_dir, state)
    print("[testbench] completed; see summary.md and results.json")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

