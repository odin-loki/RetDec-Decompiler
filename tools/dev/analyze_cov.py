#!/usr/bin/env python3
import subprocess, json, sys, os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("RETDEC_BUILD", REPO_ROOT / "build" / "linux")).expanduser()
SRCDIR = str(REPO_ROOT)

result = subprocess.run([
    "python3", "-m", "gcovr",
    "--root", SRCDIR,
    "--object-directory", str(BUILD / "src/llvmir2hll"),
    "--exclude", r".*external.*",
    "--exclude", r".*/usr/include/.*",
    "--exclude", r".*/retdec-build/.*",
    "--json-summary-pretty",
], capture_output=True, text=True)

if not result.stdout.strip():
    print("STDERR:", result.stderr[:300])
    sys.exit(1)
data = json.loads(result.stdout)
files = [f for f in data.get("files", [])
         if "src/llvmir2hll" in f["filename"] and f["filename"].endswith(".cpp")]

files.sort(key=lambda f: (f.get("branch_covered", 0) / max(f.get("branch_total", 1), 1)))

print(f"{'File':<58} {'Line%':>6} {'Br%':>6} {'Uncov':>6}")
print("-" * 82)
for f in files[:60]:
    name = f["filename"].replace(SRCDIR + "/", "")[-57:]
    lc, lt = f.get("line_covered", 0), f.get("line_total", 0)
    bc, bt = f.get("branch_covered", 0), f.get("branch_total", 0)
    lp = lc/lt*100 if lt else 100
    bp = bc/bt*100 if bt else 100
    print(f"{name:<58} {lp:>5.1f}% {bp:>5.1f}% {bt-bc:>6}")

print("-" * 82)
tbc = sum(f.get("branch_covered", 0) for f in files)
tbt = sum(f.get("branch_total", 0) for f in files)
tlc = sum(f.get("line_covered", 0) for f in files)
tlt = sum(f.get("line_total", 0) for f in files)
print(f"TOTAL (src/llvmir2hll): lines {tlc}/{tlt} ({tlc/tlt*100 if tlt else 0:.1f}%), "
      f"branches {tbc}/{tbt} ({tbc/tbt*100 if tbt else 0:.1f}%)")
print(f"Uncovered branches: {tbt-tbc}")
