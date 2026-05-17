#!/usr/bin/env python3
"""
gui_debug_driver.py — Automated graphical debugging driver for retdec-gui.

Goals
-----
1. Launch retdec-gui (optionally with a binary to open) under a native
   debugger so any crash or hang is captured with a stack trace and a
   minidump / core.
2. Stream the child's stdout+stderr to disk in real time so we always see
   what the GUI thought it was doing.
3. Stop the child if it exceeds --timeout seconds, and dump state before
   exiting.
4. Emit a single JSON report (--out/report.json) suitable for CI artifacts.

Design notes
------------
* Standard library only — no pip install needed on CI.
* Windows: prefers cdb.exe (Windows SDK Debuggers), falls back to no-debugger
  mode with WER-style minidump via Task Manager API (best effort).
* Linux:  prefers gdb -batch -ex run -ex bt with a tmp script.
* macOS:  prefers lldb -o run -o bt -o quit.
* Auto-detects retdec-gui next to this script (../../build/<plat>/src/gui)
  or honours --gui-exe.
* In headless mode (default) we use Qt offscreen via the GUI's built-in
  --headless plus --headless-exit-ms, so the run is reproducible on
  truly headless boxes.

Example invocations
-------------------
  # Smoke test with timeout, no binary, headless, 5 s timeout
  python gui_debug_driver.py --headless-exit-ms 5000 --timeout 30

  # Decompile a file under debugger
  python gui_debug_driver.py --binary path/to/sample.exe --timeout 600

  # Force gdb on Linux, save dump
  python gui_debug_driver.py --debugger gdb --out ./gui-debug-artifacts
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


# ─── Logging ─────────────────────────────────────────────────────────────────

def log(msg: str) -> None:
    """Single-line, timestamped log to stderr."""
    ts = time.strftime("%H:%M:%S")
    print(f"[gui-dbg {ts}] {msg}", file=sys.stderr, flush=True)


# ─── Data ────────────────────────────────────────────────────────────────────

@dataclass
class Report:
    gui_exe: str = ""
    binary: str = ""
    debugger: str = ""
    debugger_path: str = ""
    pid: int = 0
    exit_code: int = -1
    duration_sec: float = 0.0
    timeout: bool = False
    artifacts: List[str] = field(default_factory=list)
    backtrace_summary: List[str] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)

    def add_note(self, n: str) -> None:
        log(n)
        self.notes.append(n)


# ─── Path resolution ─────────────────────────────────────────────────────────

def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parent.parent.parent  # scripts/python/<this>


def autodetect_gui_exe() -> Optional[Path]:
    """Look in the most common build trees the project creates."""
    root = repo_root_from_script()
    candidates: List[Path] = []
    bin_name = "retdec-gui.exe" if platform.system() == "Windows" else "retdec-gui"

    for sub in ("build/windows/src/gui", "build/linux/src/gui",
                "build/src/gui", "build/Debug/src/gui",
                "build/Release/src/gui", "install/bin",
                "build/windows/install/bin", "build/linux/install/bin"):
        p = root / sub / bin_name
        if p.is_file():
            candidates.append(p)
    if candidates:
        candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        return candidates[0]
    on_path = shutil.which(bin_name)
    if on_path:
        return Path(on_path)
    return None


def autodetect_debugger(prefer: str) -> Tuple[str, Optional[Path]]:
    """Return (kind, path).  kind is one of {'cdb','gdb','lldb','none'}."""
    if prefer == "none":
        return "none", None
    sysname = platform.system()
    candidates: List[Tuple[str, str]] = []
    if prefer == "auto":
        if sysname == "Windows":
            candidates.extend([("cdb", "cdb.exe")])
        elif sysname == "Darwin":
            candidates.extend([("lldb", "lldb"), ("gdb", "gdb")])
        else:
            candidates.extend([("gdb", "gdb"), ("lldb", "lldb")])
    else:
        suffix = ".exe" if sysname == "Windows" else ""
        candidates.append((prefer, prefer + suffix))
    for kind, exe in candidates:
        p = shutil.which(exe)
        if p:
            return kind, Path(p)
    # Fall back: try common SDK install dir for cdb on Windows.
    if sysname == "Windows" and prefer in ("auto", "cdb"):
        for guess in (
            r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
            r"C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe",
        ):
            if Path(guess).is_file():
                return "cdb", Path(guess)
    return "none", None


# ─── Debugger launch helpers ─────────────────────────────────────────────────

def build_cdb_command(cdb: Path, gui: Path, gui_args: List[str], dump: Path,
                      log_file: Path, timeout: int) -> List[str]:
    """
    cdb -g -G runs to completion, captures any fault to the log.
    .dump /ma writes a full minidump on second-chance exceptions.
    """
    # On Windows we put the .dump command into a script file because escaping
    # quoting on Windows commandline is fiddly.
    script_path = log_file.with_suffix(".cdb-script")
    script_path.write_text(
        textwrap.dedent(
            f"""
            sxe -c "" gp
            sxe -c ".dump /ma {dump.as_posix()};qd" av
            sxe -c ".dump /ma {dump.as_posix()};qd" eh
            sxe -c ".dump /ma {dump.as_posix()};qd" sov
            .lines -e
            g
            qd
            """
        ).strip()
        + "\n",
        encoding="ascii",
    )
    return [
        str(cdb),
        "-g",           # ignore initial breakpoint
        "-G",           # ignore exit breakpoint
        "-cf", str(script_path),
        "-logo", str(log_file),
        "-c", "g",
        str(gui),
        *gui_args,
    ]


def build_gdb_command(gdb: Path, gui: Path, gui_args: List[str],
                      log_file: Path, core_dir: Path, timeout: int) -> List[str]:
    script = log_file.with_suffix(".gdb-script")
    script.write_text(
        textwrap.dedent(
            f"""
            set pagination off
            set logging file {log_file.as_posix()}
            set logging overwrite on
            set logging redirect off
            set logging enabled on
            set confirm off
            handle SIGSEGV stop print nopass
            handle SIGABRT stop print nopass
            run
            info threads
            thread apply all bt full
            generate-core-file {(core_dir / "retdec-gui.core").as_posix()}
            quit
            """
        ).strip()
        + "\n",
        encoding="ascii",
    )
    return [
        str(gdb),
        "-batch",
        "-x", str(script),
        "--args",
        str(gui),
        *gui_args,
    ]


def build_lldb_command(lldb: Path, gui: Path, gui_args: List[str],
                       log_file: Path, core_dir: Path) -> List[str]:
    script = log_file.with_suffix(".lldb-script")
    script.write_text(
        textwrap.dedent(
            f"""
            settings set auto-confirm true
            settings set inferior-tty-name /dev/null
            run
            bt all
            process save-core {(core_dir / "retdec-gui.core").as_posix()}
            quit
            """
        ).strip()
        + "\n",
        encoding="ascii",
    )
    return [
        str(lldb),
        "-b",
        "-s", str(script),
        "--",
        str(gui),
        *gui_args,
    ]


# ─── Output capture ──────────────────────────────────────────────────────────

def stream_to_file(proc: subprocess.Popen, sink: Path, also_stderr: bool) -> None:
    """Pipe combined stdout/stderr from `proc` to `sink` line-by-line."""
    # subprocess already gave us a single PIPE; we just copy bytes.
    assert proc.stdout is not None
    with open(sink, "ab") as f:
        while True:
            chunk = proc.stdout.read(4096)
            if not chunk:
                break
            f.write(chunk)
            f.flush()


# ─── Backtrace parsing ───────────────────────────────────────────────────────

def parse_backtrace(log_path: Path) -> List[str]:
    """Cheap line filter that pulls obvious frame lines from cdb/gdb/lldb logs."""
    if not log_path.is_file():
        return []
    text = log_path.read_text(encoding="utf-8", errors="replace")
    frames: List[str] = []
    # gdb / lldb frames look like "#42  0xdeadbeef in symbol ..."
    for m in re.finditer(r"^\s*#\d+\s+[^\n]+", text, re.MULTILINE):
        frames.append(m.group(0).strip())
    # cdb stack lines look like "01 0x… retdec_gui!RetDecMainWindow::…"
    for m in re.finditer(r"^\s*[0-9a-f]{2}\s+[0-9a-f`]+\s+\S+!\S+", text,
                          re.MULTILINE):
        frames.append(m.group(0).strip())
    # Limit so JSON reports stay sane.
    return frames[:200]


# ─── Main driver ─────────────────────────────────────────────────────────────

def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run retdec-gui under a debugger and capture crashes.",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument("--gui-exe", help="Path to retdec-gui (default: auto-detect)")
    parser.add_argument("--binary", help="Binary or .retdec project to open in the GUI")
    parser.add_argument("--out", default="gui-debug-artifacts",
                        help="Output directory for log/dumps/report (default: gui-debug-artifacts)")
    parser.add_argument("--timeout", type=int, default=120,
                        help="Wall-clock timeout in seconds (default: 120)")
    parser.add_argument("--debugger", default="auto",
                        choices=("auto", "cdb", "gdb", "lldb", "none"))
    parser.add_argument("--asan", action="store_true",
                        help="Configure the child for AddressSanitizer "
                             "(sets ASAN_OPTIONS, expects an ASAN-built GUI).")
    parser.add_argument("--pageheap", action="store_true",
                        help="Windows: try to enable PageHeap for the child "
                             "via the _NT_GLOBAL_FLAG env override (catches "
                             "most heap corruption without gflags.exe).")
    parser.add_argument("--headless-exit-ms", type=int, default=4000,
                        help="Pass --headless-exit-ms N to the GUI (default: 4000; 0 disables)")
    parser.add_argument("--no-headless", action="store_true",
                        help="Do not request Qt offscreen mode (debug visible UI)")
    parser.add_argument("--extra-arg", action="append", default=[],
                        help="Extra positional/flag argument forwarded to retdec-gui (repeatable)")
    args = parser.parse_args(list(argv) if argv is not None else None)

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    log_path = out / "gui.log"
    dump_path = out / "retdec-gui.dmp"
    report_path = out / "report.json"

    report = Report()

    # Resolve GUI.
    gui_exe = Path(args.gui_exe).resolve() if args.gui_exe else autodetect_gui_exe()
    if not gui_exe or not gui_exe.is_file():
        report.add_note(f"retdec-gui not found (searched build trees and PATH); "
                        f"pass --gui-exe.")
        report_path.write_text(json.dumps(asdict(report), indent=2), encoding="utf-8")
        return 2
    report.gui_exe = str(gui_exe)
    log(f"Using GUI: {gui_exe}")

    # Resolve debugger.
    dbg_kind, dbg_path = autodetect_debugger(args.debugger)
    report.debugger = dbg_kind
    report.debugger_path = str(dbg_path) if dbg_path else ""
    if dbg_kind == "none" and args.debugger not in ("auto", "none"):
        report.add_note(f"Requested debugger {args.debugger} not found; running without one.")
    elif dbg_kind == "none":
        report.add_note("No debugger detected; running without one (still captures stdout).")
    else:
        log(f"Using debugger: {dbg_kind} ({dbg_path})")

    # Build GUI argv.
    gui_args: List[str] = []
    if not args.no_headless:
        gui_args.append("--headless")
        if args.headless_exit_ms > 0:
            gui_args += ["--headless-exit-ms", str(args.headless_exit_ms)]
    if args.binary:
        gui_args.append(args.binary)
        report.binary = args.binary
    gui_args += [a for a in args.extra_arg if a]

    # Build command line.
    cmd: List[str]
    if dbg_kind == "cdb" and dbg_path:
        cmd = build_cdb_command(dbg_path, gui_exe, gui_args, dump_path, log_path,
                                args.timeout)
    elif dbg_kind == "gdb" and dbg_path:
        cmd = build_gdb_command(dbg_path, gui_exe, gui_args, log_path, out,
                                args.timeout)
    elif dbg_kind == "lldb" and dbg_path:
        cmd = build_lldb_command(dbg_path, gui_exe, gui_args, log_path, out)
    else:
        cmd = [str(gui_exe), *gui_args]

    # On Linux make sure core files are allowed.
    if platform.system() == "Linux":
        try:
            import resource  # type: ignore[import-not-found]
            resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY,
                                                     resource.RLIM_INFINITY))
        except Exception:
            pass

    # Environment for headless runs.
    env = os.environ.copy()
    if not args.no_headless and "QT_QPA_PLATFORM" not in env:
        env["QT_QPA_PLATFORM"] = "offscreen"
    env.setdefault("QT_LOGGING_RULES", "qt.qpa.window=false")

    # AddressSanitizer wiring: route reports to stderr / sink with verbose
    # symbolisation. The GUI must have been built with -DRETDEC_GUI_ASAN=ON.
    if args.asan:
        existing = env.get("ASAN_OPTIONS", "")
        asan_opts = [
            "halt_on_error=0",
            "abort_on_error=0",
            "print_stats=1",
            "fast_unwind_on_malloc=0",
            "symbolize=1",
        ]
        # LSan (detect_leaks) is supported on Linux/macOS but not on Windows
        # ASAN; emitting it there makes ASAN refuse to start.
        if platform.system() != "Windows":
            asan_opts.append("detect_leaks=1")
        if existing:
            asan_opts.insert(0, existing)
        env["ASAN_OPTIONS"] = ":".join(asan_opts)
        env.setdefault("ASAN_SYMBOLIZER_PATH",
                       shutil.which("llvm-symbolizer") or "")
        report.add_note("ASAN_OPTIONS=" + env["ASAN_OPTIONS"])

    # Windows PageHeap (no gflags.exe needed): set per-process via
    # `_NT_GLOBAL_FLAG` env (decimal 0x02000000 = hpa). This only takes
    # effect on Windows when the loader sees it before CRT init.
    if args.pageheap and platform.system() == "Windows":
        # hpa = 0x02000000  (Enable page heap)
        # htg = 0x00001000  (Enable heap tagging by DLL)
        flag = int(env.get("_NT_GLOBAL_FLAG", "0") or "0", 0) | 0x02000000 | 0x00001000
        env["_NT_GLOBAL_FLAG"] = hex(flag)
        report.add_note(f"PageHeap requested: _NT_GLOBAL_FLAG={env['_NT_GLOBAL_FLAG']}")

    log(f"Launching: {' '.join(cmd)}")
    started = time.monotonic()

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            cwd=str(gui_exe.parent),
        )
    except OSError as e:
        report.add_note(f"Failed to spawn: {e}")
        report_path.write_text(json.dumps(asdict(report), indent=2), encoding="utf-8")
        return 3

    report.pid = proc.pid
    timed_out = False
    try:
        deadline = started + args.timeout
        assert proc.stdout is not None
        with open(log_path, "ab") as logf:
            while True:
                # Read up to 4 KiB, with a non-blocking poll using select on POSIX
                # or a small sleep on Windows (Win32 anonymous pipes don't select).
                chunk = b""
                try:
                    chunk = proc.stdout.read1(4096) if hasattr(proc.stdout, "read1") \
                            else proc.stdout.read(4096)
                except OSError:
                    chunk = b""
                if chunk:
                    logf.write(chunk)
                    logf.flush()
                else:
                    if proc.poll() is not None:
                        break
                    time.sleep(0.05)

                if time.monotonic() > deadline:
                    timed_out = True
                    report.add_note(f"Timeout hit at {args.timeout}s — terminating.")
                    break
    except KeyboardInterrupt:
        report.add_note("KeyboardInterrupt — terminating child.")

    # Tear down.
    if proc.poll() is None:
        try:
            if platform.system() == "Windows":
                proc.terminate()
            else:
                proc.send_signal(signal.SIGTERM)
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    report.exit_code = proc.returncode if proc.returncode is not None else -1
    report.duration_sec = round(time.monotonic() - started, 3)
    report.timeout = timed_out

    if log_path.is_file():
        report.artifacts.append(str(log_path))
    if dump_path.is_file():
        report.artifacts.append(str(dump_path))
    for child in out.iterdir():
        if child.suffix in (".core",) and str(child) not in report.artifacts:
            report.artifacts.append(str(child))

    report.backtrace_summary = parse_backtrace(log_path)

    report_path.write_text(json.dumps(asdict(report), indent=2), encoding="utf-8")
    log(f"Report: {report_path}")
    log(f"Exit code: {report.exit_code} (timeout={report.timeout}, duration={report.duration_sec}s)")

    # Non-zero only on clear failure.
    if report.timeout:
        return 124
    if report.exit_code != 0:
        return report.exit_code if report.exit_code > 0 else 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
