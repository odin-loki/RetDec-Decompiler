import os
import struct
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("RETDEC_BUILD", REPO_ROOT / "build" / "linux")).expanduser()
OBJ = BUILD / "src/llvmir2hll/CMakeFiles/llvmir2hll.dir/graphs/cfg"
gcno = OBJ / "cfg.cpp.gcno"
gcda = OBJ / "cfg.cpp.gcda"
with open(gcno, "rb") as f:
    magic = f.read(4)
    version = f.read(4)
    stamp = struct.unpack("<I", f.read(4))[0]
    print(f"GCNO stamp: {hex(stamp)}")
with open(gcda, "rb") as f:
    magic = f.read(4)
    version = f.read(4)
    stamp = struct.unpack("<I", f.read(4))[0]
    print(f"GCDA stamp: {hex(stamp)}")
