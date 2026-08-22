# Q4 goto-optimizer baseline

Default F5 `.c` (not `.buildable.c`) on ci-core stems at gcc O0/O2/O3.
Counts the `goto` token. This is the pre-SAILR baseline; not a SAILR port.

- samples: 27
- total goto: **39**
- mean goto: **1.44**

| Binary | goto | bytes |
|--------|------|-------|
| `bubblesort-gcc-O0` | 0 | 5290 |
| `bubblesort-gcc-O2` | 0 | 4718 |
| `bubblesort-gcc-O3` | 0 | 4230 |
| `mergesort-gcc-O0` | 0 | 7355 |
| `mergesort-gcc-O2` | 3 | 8033 |
| `mergesort-gcc-O3` | 15 | 15380 |
| `hash_table-gcc-O0` | 0 | 6884 |
| `hash_table-gcc-O2` | 0 | 6918 |
| `hash_table-gcc-O3` | 0 | 6918 |
| `ring_buffer-gcc-O0` | 0 | 5402 |
| `ring_buffer-gcc-O2` | 0 | 4452 |
| `ring_buffer-gcc-O3` | 0 | 4822 |
| `binary_search-gcc-O0` | 0 | 5164 |
| `binary_search-gcc-O2` | 4 | 5245 |
| `binary_search-gcc-O3` | 4 | 5096 |
| `memcpy_loop-gcc-O0` | 0 | 4463 |
| `memcpy_loop-gcc-O2` | 0 | 3777 |
| `memcpy_loop-gcc-O3` | 0 | 3777 |
| `generated_quicksort-gcc-O0` | 0 | 6036 |
| `generated_quicksort-gcc-O2` | 0 | 5727 |
| `generated_quicksort-gcc-O3` | 0 | 5727 |
| `generated_heapsort-gcc-O0` | 0 | 6573 |
| `generated_heapsort-gcc-O2` | 4 | 6593 |
| `generated_heapsort-gcc-O3` | 9 | 7548 |
| `generated_pthread_mutex-gcc-O0` | 0 | 5371 |
| `generated_pthread_mutex-gcc-O2` | 0 | 5321 |
| `generated_pthread_mutex-gcc-O3` | 0 | 5321 |
