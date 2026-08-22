# B8 loop-containing negatives

FIR / histogram / Bresenham / box-blur / HTTP-verb / UTF-8 scan /
sliding-max / transpose / saturating-add / dot-product. Empty labels.
These *do* have loops, so they stress `strlen` / `atoi` / `dfs` heuristics
and the sort opcode bag. Any extracted label is a false positive.

- binaries requested: 100
- decompiled: 100
- binaries with any label: **80**
- false-positive rate: **0.800**

Remasured after `QuicksortDetector` required a recursive self-call.
`sort:quicksort` is gone. The same 80 binaries now get
`sort:mergesort (std::stable_sort)` at 0.55 from the merge-loop
floor (`loads>=2 && cmps>=1 && cbs>=2`). Binary FP is unchanged.

A4: confidences are **not fitted**. The table below is an observation
on this negative set only. Detector constants were not changed.

## False-positive confidence (A4 observation)

| Detection | n | mean conf | min | max |
|-----------|---|-----------|-----|-----|
| `algorithm:std::copy` | 10 | 0.850 | 0.850 | 0.850 |
| `algorithm:std::transform` | 70 | 0.850 | 0.850 | 0.850 |
| `container:ring_buffer` | 10 | 1.000 | 1.000 | 1.000 |
| `container:std::unordered_map<uint32_t, uint32_t>` | 70 | 0.450 | 0.450 | 0.450 |
| `sort:mergesort (std::stable_sort)` | 80 | 0.550 | 0.550 | 0.550 |

## Positives (false positives)

- `box_blur_00-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_01-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_02-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_03-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_04-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_05-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_06-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_07-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_08-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `box_blur_09-gcc-O0`: CircularBuffer, Mergesort, RingBuffer, Sort
- `dot3_00-gcc-O0`: Mergesort, Sort
- `dot3_01-gcc-O0`: Mergesort, Sort
- `dot3_02-gcc-O0`: Mergesort, Sort
- `dot3_03-gcc-O0`: Mergesort, Sort
- `dot3_04-gcc-O0`: Mergesort, Sort
- `dot3_05-gcc-O0`: Mergesort, Sort
- `dot3_06-gcc-O0`: Mergesort, Sort
- `dot3_07-gcc-O0`: Mergesort, Sort
- `dot3_08-gcc-O0`: Mergesort, Sort
- `dot3_09-gcc-O0`: Mergesort, Sort
- `fir_tap_00-gcc-O0`: Mergesort, Sort
- `fir_tap_01-gcc-O0`: Mergesort, Sort
- `fir_tap_02-gcc-O0`: Mergesort, Sort
- `fir_tap_03-gcc-O0`: Mergesort, Sort
- `fir_tap_04-gcc-O0`: Mergesort, Sort
- `fir_tap_05-gcc-O0`: Mergesort, Sort
- `fir_tap_06-gcc-O0`: Mergesort, Sort
- `fir_tap_07-gcc-O0`: Mergesort, Sort
- `fir_tap_08-gcc-O0`: Mergesort, Sort
- `fir_tap_09-gcc-O0`: Mergesort, Sort
- `hist16_00-gcc-O0`: Mergesort, Sort
- `hist16_01-gcc-O0`: Mergesort, Sort
- `hist16_02-gcc-O0`: Mergesort, Sort
- `hist16_03-gcc-O0`: Mergesort, Sort
- `hist16_04-gcc-O0`: Mergesort, Sort
- `hist16_05-gcc-O0`: Mergesort, Sort
- `hist16_06-gcc-O0`: Mergesort, Sort
- `hist16_07-gcc-O0`: Mergesort, Sort
- `hist16_08-gcc-O0`: Mergesort, Sort
- `hist16_09-gcc-O0`: Mergesort, Sort
- `http_verb_00-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_01-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_02-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_03-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_04-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_05-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_06-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_07-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_08-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `http_verb_09-gcc-O0`: Copy, Memcpy, Mergesort, Sort
- `sliding_max_00-gcc-O0`: Mergesort, Sort
- `sliding_max_01-gcc-O0`: Mergesort, Sort
- `sliding_max_02-gcc-O0`: Mergesort, Sort
- `sliding_max_03-gcc-O0`: Mergesort, Sort
- `sliding_max_04-gcc-O0`: Mergesort, Sort
- `sliding_max_05-gcc-O0`: Mergesort, Sort
- `sliding_max_06-gcc-O0`: Mergesort, Sort
- `sliding_max_07-gcc-O0`: Mergesort, Sort
- `sliding_max_08-gcc-O0`: Mergesort, Sort
- `sliding_max_09-gcc-O0`: Mergesort, Sort
- `transpose4_00-gcc-O0`: Mergesort, Sort
- `transpose4_01-gcc-O0`: Mergesort, Sort
- `transpose4_02-gcc-O0`: Mergesort, Sort
- `transpose4_03-gcc-O0`: Mergesort, Sort
- `transpose4_04-gcc-O0`: Mergesort, Sort
- `transpose4_05-gcc-O0`: Mergesort, Sort
- `transpose4_06-gcc-O0`: Mergesort, Sort
- `transpose4_07-gcc-O0`: Mergesort, Sort
- `transpose4_08-gcc-O0`: Mergesort, Sort
- `transpose4_09-gcc-O0`: Mergesort, Sort
- `utf8_scan_00-gcc-O0`: Mergesort, Sort
- `utf8_scan_01-gcc-O0`: Mergesort, Sort
- `utf8_scan_02-gcc-O0`: Mergesort, Sort
- `utf8_scan_03-gcc-O0`: Mergesort, Sort
- `utf8_scan_04-gcc-O0`: Mergesort, Sort
- `utf8_scan_05-gcc-O0`: Mergesort, Sort
- `utf8_scan_06-gcc-O0`: Mergesort, Sort
- `utf8_scan_07-gcc-O0`: Mergesort, Sort
- `utf8_scan_08-gcc-O0`: Mergesort, Sort
- `utf8_scan_09-gcc-O0`: Mergesort, Sort
