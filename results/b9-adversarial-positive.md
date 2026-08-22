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
- mean F1: **0.076** (95% CI 0.000–0.184)
- micro F1: **0.099** (tp=4 fp=47 fn=26)
- only assigned hit: sentinel heapsort (O0 0.571, O2 0.800)
- BFS / DFS / atoi / strlen / varint / AES: **0** name-blind F1
- frequent false labels: `QuickSort` / `Sort` on non-sort binaries

## Per binary

| Binary | Expected | Predicted | F1 |
|--------|----------|-----------|----|
| `aes_bitslice-gcc-O0` | AES | Copy, HashTable, Memcpy, OpenAddressing, QuickSort, Sort | 0.000 |
| `aes_bitslice-gcc-O2` | AES | HashTable, HeapSort, OpenAddressing, QuickSort, Sort | 0.000 |
| `aes_ni-gcc-O0` | AES | QuickSort, Sort | 0.000 |
| `aes_ni-gcc-O2` | AES | (none) | 0.000 |
| `aes_ttable-gcc-O0` | AES | HashTable, OpenAddressing, QuickSort, Sort | 0.000 |
| `aes_ttable-gcc-O2` | AES | HashTable, HeapSort, OpenAddressing, Sort | 0.000 |
| `atoi_hex_table-gcc-O0` | Atoi, Parse | Copy, Memcpy, QuickSort, Sort | 0.000 |
| `atoi_hex_table-gcc-O2` | Atoi, Parse | HeapSort, Sort | 0.000 |
| `bfs_ring-gcc-O0` | BFS, GraphTraversal | QuickSort, Sort | 0.000 |
| `bfs_ring-gcc-O2` | BFS, GraphTraversal | QuickSort, Sort | 0.000 |
| `dfs_explicit_stack-gcc-O0` | DFS, GraphTraversal | Copy, Memcpy, QuickSort, Sort | 0.000 |
| `dfs_explicit_stack-gcc-O2` | DFS, GraphTraversal | QuickSort, Sort | 0.000 |
| `heapsort_sentinel-gcc-O0` | HeapSort, Sort | CircularBuffer, HeapSort, QuickSort, RingBuffer, Sort | 0.571 |
| `heapsort_sentinel-gcc-O2` | HeapSort, Sort | HeapSort, QuickSort, Sort | 0.800 |
| `strlen_word-gcc-O0` | Strlen, String | QuickSort, Sort | 0.000 |
| `strlen_word-gcc-O2` | Strlen, String | QuickSort, Sort | 0.000 |
| `varint_unrolled-gcc-O0` | Varint, Serialization | QuickSort, Sort | 0.000 |
| `varint_unrolled-gcc-O2` | Varint, Serialization | (none) | 0.000 |

## AES evidence in `.config.json`

- `aes_bitslice-gcc-O0`: none
- `aes_bitslice-gcc-O2`: none
- `aes_ni-gcc-O0`: none
- `aes_ni-gcc-O2`: none
- `aes_ttable-gcc-O0`: none
- `aes_ttable-gcc-O2`: none

