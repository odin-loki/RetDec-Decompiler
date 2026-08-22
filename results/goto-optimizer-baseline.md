# Q4 goto-optimizer baseline

Default F5 `.c` (not `.buildable.c`) on ci-core 9 **gcc-O0**. Counts the `goto` token.
This is the pre-SAILR baseline; do not treat it as a SAILR port.
O2/O3 and unstructured CFGs can still emit `goto` (see the emit-buildable
missing-label / orphan-`break` sidecars). Full 216 was not re-counted.

- samples: 9
- total goto: **0**
- mean goto: **0.00**

| Binary | goto | bytes |
|--------|------|-------|
| `bubblesort-gcc-O0` | 0 | 5290 |
| `mergesort-gcc-O0` | 0 | 7355 |
| `hash_table-gcc-O0` | 0 | 6884 |
| `ring_buffer-gcc-O0` | 0 | 5402 |
| `binary_search-gcc-O0` | 0 | 5164 |
| `memcpy_loop-gcc-O0` | 0 | 4463 |
| `generated_quicksort-gcc-O0` | 0 | 6036 |
| `generated_heapsort-gcc-O0` | 0 | 6573 |
| `generated_pthread_mutex-gcc-O0` | 0 | 5371 |
