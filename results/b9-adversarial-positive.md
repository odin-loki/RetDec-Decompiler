# B9 adversarial-positive corpus

Idiosyncratic implementations of target algorithms (audit B9):
heapsort with a 1-based sentinel, BFS with a ring buffer, iterative DFS,
table atoi, SWAR strlen, unrolled varint, AES T-tables, algebraic AES
(GF inversion, no S-box array), and AES-NI.

Scored name-blind (`--no-stem-fallback`). This is **recall on hard positives**,
not a product F1. `crypto_detect` is **not** merged into decompiler
`semanticDetections` (no public-header change to `FunctionDetections`).
AES rows are therefore expected misses unless `usedCryptoConstants` is set.

- binaries: **18**
- mean F1: **0.111** (heapsort needs Mul 2 / Shl 1; strlen dropped)
- micro F1: **0.174** (tp=4 fp=12 fn=26)

## Per binary

| Binary | Expected | Predicted | F1 |
|--------|----------|-----------|----|
| `aes_bitslice-gcc-O0` | AES | Copy, Memcpy | 0.000 |
| `aes_bitslice-gcc-O2` | AES | HeapSort, Sort | 0.000 |
| `aes_ni-gcc-O0` | AES | (none) | 0.000 |
| `aes_ni-gcc-O2` | AES | (none) | 0.000 |
| `aes_ttable-gcc-O0` | AES | (none) | 0.000 |
| `aes_ttable-gcc-O2` | AES | HeapSort, Sort | 0.000 |
| `atoi_hex_table-gcc-O0` | Atoi, Parse | Copy, Memcpy | 0.000 |
| `atoi_hex_table-gcc-O2` | Atoi, Parse | InsertionSort, Sort | 0.000 |
| `bfs_ring-gcc-O0` | BFS, GraphTraversal | (none) | 0.000 |
| `bfs_ring-gcc-O2` | BFS, GraphTraversal | (none) | 0.000 |
| `dfs_explicit_stack-gcc-O0` | DFS, GraphTraversal | Copy, Memcpy | 0.000 |
| `dfs_explicit_stack-gcc-O2` | DFS, GraphTraversal | (none) | 0.000 |
| `heapsort_sentinel-gcc-O0` | HeapSort, Sort | HeapSort, Sort | 1.000 |
| `heapsort_sentinel-gcc-O2` | HeapSort, Sort | HeapSort, Sort | 1.000 |
| `strlen_word-gcc-O0` | Strlen, String | (none) | 0.000 |
| `strlen_word-gcc-O2` | Strlen, String | (none) | 0.000 |
| `varint_unrolled-gcc-O0` | Varint, Serialization | (none) | 0.000 |
| `varint_unrolled-gcc-O2` | Varint, Serialization | (none) | 0.000 |

## AES evidence in `.config.json`

- `aes_bitslice-gcc-O0`: none
- `aes_bitslice-gcc-O2`: none
- `aes_ni-gcc-O0`: none
- `aes_ni-gcc-O2`: none
- `aes_ttable-gcc-O0`: none
- `aes_ttable-gcc-O2`: none

