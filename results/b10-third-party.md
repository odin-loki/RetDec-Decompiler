# B10 third-party corpus

Headline this run: zlib 1.3.1 **crc32.c only** (no deflate/trees).
Labels are taken from that upstream source, not from the detector.
The larger crc+deflate pair previously **timed out at 300s** and is
not re-run here. This is **not** a full Debian coreutils/OpenSSL/SQLite set.

Name-blind (`--no-stem-fallback`). CRC is not an assigned `IdiomDetector`
kind (same rule as A6). Expect low recall.

- binaries scored this run: **2** (crc32.c only; 2/2 decompiled)
- mean F1: **0.000** (tp=0; CRC is not an assigned idiom)
- micro F1: **0.000** (fp=17 fn=4)
- crc+deflate pair: still the prior **300s timeout** (not re-run)

| Binary | Expected | Predicted | F1 |
|--------|----------|-----------|----|
| `zlib_crc_only-gcc-O0` | CRC, Checksum | CircularBuffer, Copy, HashTable, Memcpy, OpenAddressing, QuickSort, RingBuffer, Sort | 0.000 |
| `zlib_crc_only-gcc-O2` | CRC, Checksum | CircularBuffer, Copy, HashTable, HeapSort, Memcpy, OpenAddressing, QuickSort, RingBuffer, Sort | 0.000 |

