#!/usr/bin/env bash
# B10: compile a small zlib subset from a pinned upstream tarball.
# Labels come from zlib crc32.c / compress.c, not from our detectors.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/tests/algorithm_recovery/third_party_corpus"
SRC_OUT="${ROOT}/tests/algorithm_recovery/sources/third_party"
VENDOR="${ROOT}/build/linux/third_party/zlib-src"
URL="https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz"
SHA="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
mkdir -p "${OUT}" "${SRC_OUT}" "${VENDOR}"

cat > "${SRC_OUT}/zlib_crc_compress.c" <<'C'
/* Driver only. Labels are from upstream zlib crc32.c / compress.c. */
#include <stdio.h>
#include "zlib.h"

int main(void)
{
	const unsigned char d[] = "third-party zlib corpus";
	unsigned long crc = crc32(0L, d, sizeof d - 1);
	unsigned long dest_len = 128;
	unsigned char out[128];
	if (compress(out, &dest_len, d, sizeof d - 1) != Z_OK) return 2;
	printf("%lu %lu\n", crc, dest_len);
	return (int)(crc & 255);
}
C

cat > "${SRC_OUT}/zlib_crc_compress.labels.json" <<'JSON'
{
  "algorithms": ["CRC", "Checksum", "Compression"],
  "provenance": "madler zlib 1.3.1 crc32.c and compress.c",
  "functions": {}
}
JSON

cat > "${SRC_OUT}/zlib_crc_only.c" <<'C'
/* Driver only. Labels are from upstream zlib crc32.c (no deflate). */
#include <stdio.h>
#include "zlib.h"

int main(void)
{
	const unsigned char d[] = "third-party zlib crc32 only";
	unsigned long crc = crc32(0L, d, sizeof d - 1);
	printf("%lu\n", crc);
	return (int)(crc & 255);
}
C

cat > "${SRC_OUT}/zlib_crc_only.labels.json" <<'JSON'
{
  "algorithms": ["CRC", "Checksum"],
  "provenance": "madler zlib 1.3.1 crc32.c",
  "functions": {}
}
JSON

TARBALL="${VENDOR}/zlib-1.3.1.tar.gz"
if [[ ! -f "${VENDOR}/zlib-1.3.1/crc32.c" ]]; then
	curl -fsSL -o "${TARBALL}" "${URL}"
	echo "${SHA}  ${TARBALL}" | sha256sum -c -
	tar -xzf "${TARBALL}" -C "${VENDOR}"
fi
Z="${VENDOR}/zlib-1.3.1"
# zlib 1.3.1 ships zconf.h.in; generate a minimal zconf.h if needed.
if [[ ! -f "${Z}/zconf.h" ]]; then
	cp "${Z}/zconf.h.in" "${Z}/zconf.h"
fi

python3 - "${SRC_OUT}" "${OUT}" "${Z}" <<'PY'
import json, subprocess, sys
from pathlib import Path

src, out, z = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
cc_ver = subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0].strip()
targets = [
    {
        "driver": src / "zlib_crc_compress.c",
        "stem": "zlib_crc_compress",
        "zsrc": [
            z / "crc32.c",
            z / "adler32.c",
            z / "compress.c",
            z / "deflate.c",
            z / "trees.c",
            z / "zutil.c",
        ],
        "upstream": "zlib-1.3.1 crc32.c compress.c deflate.c",
        "source": "third_party/zlib_crc_compress.c",
    },
    {
        "driver": src / "zlib_crc_only.c",
        "stem": "zlib_crc_only",
        "zsrc": [z / "crc32.c", z / "zutil.c"],
        "upstream": "zlib-1.3.1 crc32.c only",
        "source": "third_party/zlib_crc_only.c",
    },
]
manifest = []
for spec in targets:
    for p in spec["zsrc"]:
        if not p.is_file():
            raise SystemExit(f"missing {p}")
    for opt in ("O0", "O2"):
        name = f"{spec['stem']}-gcc-{opt}"
        path = out / name
        cmd = [
            "gcc", f"-{opt}", "-std=c11", "-D_LARGEFILE64_SOURCE=1",
            f"-I{z}", "-o", str(path), str(spec["driver"]), *[str(p) for p in spec["zsrc"]],
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stderr)
            raise SystemExit(f"compile failed {name}")
        actual = path if path.is_file() else Path(str(path) + ".exe")
        subprocess.run(["strip", str(actual)], check=False)
        manifest.append({
            "name": name,
            "source": spec["source"],
            "upstream": spec["upstream"],
            "compiler": "gcc",
            "cc_version": cc_ver,
            "opt": opt,
            "path": actual.name,
            "label_source": "zlib 1.3.1 source, not the detector",
        })
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Built {len(manifest)} third-party binaries from zlib 1.3.1")
PY

python3 "${ROOT}/scripts/generate_ground_truth.py" \
	--sources "${SRC_OUT}" \
	--manifest "${OUT}/manifest.json" \
	--out "${ROOT}/tests/algorithm_recovery/ground_truth/third_party.json"
