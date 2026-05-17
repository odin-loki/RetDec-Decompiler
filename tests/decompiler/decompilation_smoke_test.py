#!/usr/bin/env python3
"""
Decompilation smoke test: run retdec-decompiler on a binary and verify output.
Usage: python3 decompilation_smoke_test.py <decompiler_path> <input_binary> [output_path]
"""
import os
import sys
import subprocess

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
            print(f"Decompiler failed (exit {result.returncode})")
            print(result.stderr[-2000:] if result.stderr else "")
            sys.exit(1)
        if not os.path.isfile(output) or os.path.getsize(output) == 0:
            print("Error: output file empty or missing")
            sys.exit(1)
        print(f"OK: {output} ({os.path.getsize(output)} bytes)")
    except subprocess.TimeoutExpired:
        print("Error: decompiler timed out")
        sys.exit(1)

if __name__ == "__main__":
    main()
