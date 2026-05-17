#!/usr/bin/env python3
"""Parse gcovr text output and show files sorted by branch coverage."""
import subprocess, sys, re, os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("RETDEC_BUILD", REPO_ROOT / "build" / "linux")).expanduser()
SRCDIR = str(REPO_ROOT)

result = subprocess.run([
    "python3", "-m", "gcovr",
    "--root", SRCDIR,
    "--object-directory", str(BUILD / "src/llvmir2hll"),
    "--object-directory", str(BUILD / "tests/llvmir2hll"),
    "--exclude", r".*external.*",
    "--exclude", r".*/usr/include/.*",
    "--exclude", r".*/retdec-build/.*",
    "--json-summary-pretty",
    "--print-summary",
], capture_output=True, text=True)

if result.returncode != 0:
    print("STDERR:", result.stderr[:500])
    sys.exit(1)

import json
data = json.loads(result.stdout)

files = data.get("files", [])
# Filter to only src/llvmir2hll source files
src_files = [f for f in files if "src/llvmir2hll" in f["filename"] and f["filename"].endswith(".cpp")]

# Sort by branch coverage ascending (0 = worst)
def branch_pct(f):
    b = f.get("branch_covered", 0)
    t = f.get("branch_total", 0)
    return (b / t * 100) if t > 0 else 100.0

src_files.sort(key=branch_pct)

print(f"{'File':<70} {'Lines%':>7} {'Branch%':>8} {'Uncov-Br':>9}")
print("-" * 100)
for f in src_files[:60]:
    fname = f["filename"].replace(SRCDIR + "/", "")
    lc = f.get("line_covered", 0)
    lt = f.get("line_total", 0)
    lpct = lc/lt*100 if lt else 100
    bc = f.get("branch_covered", 0)
    bt = f.get("branch_total", 0)
    bpct = bc/bt*100 if bt else 100
    uncov = bt - bc
    print(f"{fname:<70} {lpct:>6.1f}% {bpct:>7.1f}% {uncov:>9}")

print("-" * 100)
total_bc = sum(f.get("branch_covered", 0) for f in src_files)
total_bt = sum(f.get("branch_total", 0) for f in src_files)
total_lc = sum(f.get("line_covered", 0) for f in src_files)
total_lt = sum(f.get("line_total", 0) for f in src_files)
print(f"TOTAL (src/llvmir2hll only): lines {total_lc}/{total_lt} ({total_lc/total_lt*100:.1f}%), branches {total_bc}/{total_bt} ({total_bc/total_bt*100:.1f}%)")
