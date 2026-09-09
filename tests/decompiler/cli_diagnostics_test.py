#!/usr/bin/env python3
"""A malformed command line must fail, and say what was wrong.

Every usage error the CLI could make reported success. `printHelpAndDie()`
ends in `exit(EXIT_SUCCESS)`, and three paths reached it -- a second input
file, an option with no value, and, indirectly, an unrecognised option, which
was taken as the input file name and pushed the real one into that second-input
path. So `retdec-decompiler --ouput out.c prog.elf` printed the help text and
exited 0, having decompiled nothing, and no script or CI job could tell.

Usage: cli_diagnostics_test.py <decompiler> <input_binary>
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile

TIMEOUT = 60


def run(argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT)


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: cli_diagnostics_test.py <decompiler> <input_binary>")
        return 1
    dec, binary = sys.argv[1], sys.argv[2]
    for path in (dec, binary):
        if not os.path.isfile(path):
            print(f"Error: not found: {path}")
            return 1

    failures = []

    def expect_usage_error(desc, argv, mention=None):
        """A usage error exits non-zero and names what it did not understand."""
        try:
            r = run(argv)
        except subprocess.TimeoutExpired:
            failures.append(f"{desc}: timed out")
            return
        if r.returncode == 0:
            failures.append(
                f"{desc}: exited 0 for a command line it cannot carry out\n"
                f"  argv: {argv}\n"
                f"  stdout: {r.stdout[-400:]}"
            )
            return
        if mention is not None and mention not in (r.stdout + r.stderr):
            failures.append(
                f"{desc}: failed without naming {mention!r}\n"
                f"  stdout: {r.stdout[-400:]}\n  stderr: {r.stderr[-400:]}"
            )

    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.c")

        # An option nobody defined. This is the one that used to be taken as
        # the input file name.
        expect_usage_error(
            "unrecognised long option",
            [dec, "--definitely-not-an-option", "-o", out, binary],
            "--definitely-not-an-option",
        )
        expect_usage_error(
            "unrecognised short option",
            [dec, "-Z", "-o", out, binary],
            "-Z",
        )
        # ...and it must not have produced output on the way.
        if os.path.exists(out):
            failures.append("a rejected command line still wrote the output file")

        # Two input files: the second one is not something to ignore.
        expect_usage_error("two input files", [dec, "-o", out, binary, binary])

        # An option whose value is missing, at the end of the line.
        expect_usage_error("option with no value", [dec, binary, "-o"])

        # And the two that legitimately exit zero still do.
        for desc, argv, needle in (
            ("--help", [dec, "--help"], "Mandatory arguments"),
            ("--version", [dec, "--version"], ""),
        ):
            try:
                r = run(argv)
            except subprocess.TimeoutExpired:
                failures.append(f"{desc}: timed out")
                continue
            if r.returncode != 0:
                failures.append(f"{desc}: expected exit 0, got {r.returncode}")
            if needle and needle not in (r.stdout + r.stderr):
                failures.append(f"{desc}: output did not contain {needle!r}")

    if failures:
        print(f"cli_diagnostics: {len(failures)} case(s) failed")
        for f in failures:
            print("  " + f)
        return 1
    print("cli_diagnostics: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
