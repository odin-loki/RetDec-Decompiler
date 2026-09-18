#!/usr/bin/env python3
"""
Decompilation smoke test: run retdec-decompiler on a binary and verify output.
Usage: python3 decompilation_smoke_test.py <decompiler_path> <input_binary> [output_path]
"""
import os
import sys
import subprocess

# A crashing child says what went wrong at the TOP of its output and drags the
# stack trace along underneath. Printing only the tail is therefore printing
# everything except the cause: on Windows the first ctest-windows run to reach
# these tests reported
#
#     Decompiler failed (exit 2147483651)
#
# followed by stack frames #253 to #255, and LLVM's "Exception Code:" line --
# the one that turns 2147483651 into 0x80000003, STATUS_BREAKPOINT -- had been
# cut off the front along with whatever assertion produced it.
#
# So: head and tail, both labelled, and the whole thing written beside the
# output where a CI artefact upload can still find it.
_HEAD_LINES = 60
_TAIL_LINES = 40


def _report_child_output(label, text, dump_path=None):
    if not text:
        print(f"{label}: (empty)")
        return
    if dump_path is not None:
        try:
            with open(dump_path, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(text)
            print(f"{label}: full text in {dump_path}")
        except OSError as exc:
            print(f"{label}: could not write {dump_path}: {exc}")
    lines = text.splitlines()
    if len(lines) <= _HEAD_LINES + _TAIL_LINES:
        print(f"{label}:")
        for ln in lines:
            print("  " + ln)
        return
    print(f"{label} (first {_HEAD_LINES} lines -- the cause is here):")
    for ln in lines[:_HEAD_LINES]:
        print("  " + ln)
    print(f"{label} ... {len(lines) - _HEAD_LINES - _TAIL_LINES} lines elided ...")
    print(f"{label} (last {_TAIL_LINES} lines):")
    for ln in lines[-_TAIL_LINES:]:
        print("  " + ln)


def _describe_exit(code):
    """Windows exception codes come back as a large unsigned int; say so."""
    if code is None:
        return "none"
    unsigned = code & 0xFFFFFFFF
    if unsigned >= 0x80000000:
        return f"{code} (0x{unsigned:08X})"
    return str(code)


def main():
    if len(sys.argv) < 3:
        print("Usage: decompilation_smoke_test.py <decompiler> <input_binary> [output.c]")
        sys.exit(1)
    decompiler = sys.argv[1]
    input_bin = sys.argv[2]
    output = sys.argv[3] if len(sys.argv) > 3 else "/tmp/smoke_output.c"

    if not os.path.isfile(decompiler):
        print(f"Error: decompiler not found: {decompiler}")
        sys.exit(1)
    if not os.path.isfile(input_bin):
        print(f"Error: input binary not found: {input_bin}")
        sys.exit(1)

    try:
        result = subprocess.run(
            [decompiler, "-o", output, input_bin],
            capture_output=True, text=True, timeout=120
        )
        if result.returncode != 0:
            print(f"Decompiler failed (exit {_describe_exit(result.returncode)})")
            _report_child_output("stdout", result.stdout, output + ".stdout.log")
            _report_child_output("stderr", result.stderr, output + ".stderr.log")
            sys.exit(1)
        if not os.path.isfile(output) or os.path.getsize(output) == 0:
            print("Error: output file empty or missing")
            print(f"decompiler={decompiler}")
            print(f"input={input_bin} size={os.path.getsize(input_bin)}")
            print(f"output={output} exists={os.path.isfile(output)}")
            _report_child_output("stdout", result.stdout, output + ".stdout.log")
            _report_child_output("stderr", result.stderr, output + ".stderr.log")
            sys.exit(1)
        print(f"OK: {output} ({os.path.getsize(output)} bytes)")
    except subprocess.TimeoutExpired:
        print("Error: decompiler timed out")
        sys.exit(1)

if __name__ == "__main__":
    main()
