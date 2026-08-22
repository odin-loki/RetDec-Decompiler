# B6 rename guard (ci-core 9)

Each ELF is copied to `$(sha256)` and decompiled name-blind.
kind:label sets must match. Empty detections are allowed.

- binaries: 9
- identical: **9**
- failures: **0**

| Binary | identical | named | hashed |
|--------|-----------|-------|--------|
| `bubblesort-gcc-O0` | true | algorithm:std::copy, algorithm:std::transform, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort | algorithm:std::copy, algorithm:std::transform, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort |
| `mergesort-gcc-O0` | true | algorithm:std::partition, algorithm:std::transform, container:std::shared_ptr<t>, container:std::unordered_map<uint32_t, uint32_t>, sort:heapsort, sort:introsort (std::sort), sort:quicksort | algorithm:std::partition, algorithm:std::transform, container:std::shared_ptr<t>, container:std::unordered_map<uint32_t, uint32_t>, sort:heapsort, sort:introsort (std::sort), sort:quicksort |
| `hash_table-gcc-O0` | true | algorithm:std::copy, algorithm:std::transform, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort | algorithm:std::copy, algorithm:std::transform, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort |
| `ring_buffer-gcc-O0` | true | algorithm:std::transform, container:std::shared_ptr<t>, container:std::unordered_map<uint32_t, uint32_t>, sort:heapsort, sort:quicksort | algorithm:std::transform, container:std::shared_ptr<t>, container:std::unordered_map<uint32_t, uint32_t>, sort:heapsort, sort:quicksort |
| `binary_search-gcc-O0` | true | algorithm:std::partition, container:std::shared_ptr<t>, sort:quicksort | algorithm:std::partition, container:std::shared_ptr<t>, sort:quicksort |
| `memcpy_loop-gcc-O0` | true | algorithm:std::copy, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort | algorithm:std::copy, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort |
| `generated_quicksort-gcc-O0` | true | algorithm:dfs, algorithm:graphtraversal, algorithm:std::transform, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort | algorithm:dfs, algorithm:graphtraversal, algorithm:std::transform, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort |
| `generated_heapsort-gcc-O0` | true | algorithm:dfs, algorithm:graphtraversal, algorithm:std::transform, container:ring_buffer, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort | algorithm:dfs, algorithm:graphtraversal, algorithm:std::transform, container:ring_buffer, container:std::unordered_map<uint32_t, uint32_t>, sort:quicksort |
| `generated_pthread_mutex-gcc-O0` | true | concurrency:mutex, concurrency:thread | concurrency:mutex, concurrency:thread |

