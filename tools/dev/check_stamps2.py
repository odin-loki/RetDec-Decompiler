import os
import struct
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("RETDEC_BUILD", REPO_ROOT / "build" / "linux")).expanduser()
OBJ = BUILD / "src/llvmir2hll/CMakeFiles/llvmir2hll.dir"

files = ["analysis/use_def_analysis.cpp", "analysis/def_use_analysis.cpp"]
for f in files:
    gcno = OBJ / f"{f}.gcno"
    gcda = OBJ / f"{f}.gcda"
    if gcno.exists() and gcda.exists():
        gcno_stamp = struct.unpack("<I", gcno.read_bytes()[8:12])[0]
        gcda_stamp = struct.unpack("<I", gcda.read_bytes()[8:12])[0]
        match = "MATCH" if gcno_stamp == gcda_stamp else "MISMATCH"
        print(f"{f}: gcno={hex(gcno_stamp)} gcda={hex(gcda_stamp)} {match}")
    else:
        print(f"{f}: file missing")
