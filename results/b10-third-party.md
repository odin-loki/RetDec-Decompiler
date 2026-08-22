# B10 third-party corpus

zlib 1.3.1 (`crc32.c` / `compress.c` / `deflate.c`) compiled into a small
driver. Labels are taken from that upstream source, not from the detector.
This is **not** a full Debian coreutils/OpenSSL/SQLite set.

Name-blind (`--no-stem-fallback`). CRC is not an assigned `IdiomDetector`
kind (same rule as A6). Expect low recall.

- binaries: **2**
- decompiled: **0 / 2** (both hit the 300s extract timeout)
- mean F1: **0.000** (no predictions; not a detector score)
- micro F1: **0.000** (tp=0 fp=0 fn=6)

| Binary | Expected | Predicted | F1 |
|--------|----------|-----------|----|
| `zlib_crc_compress-gcc-O0` | CRC, Checksum, Compression | (none) | 0.000 |
| `zlib_crc_compress-gcc-O2` | CRC, Checksum, Compression | (none) | 0.000 |

